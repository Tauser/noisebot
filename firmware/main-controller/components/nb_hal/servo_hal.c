/*
 * servo_hal.c — Driver UART para servos Feetech SCS0009 via FE-TTLinker
 *
 * Protocolo SCSCL:
 *   Instrução: 0xFF 0xFF [ID] [LEN] [INSTR] [PARAMS...] [CHECKSUM]
 *   Resposta:  0xFF 0xFF [ID] [LEN] [ERROR] [PARAMS...] [CHECKSUM]
 *
 *   LEN = bytes após LEN até o final do pacote (INSTR + PARAMS + CHECKSUM).
 *   CHECKSUM = ~(ID + LEN + INSTR + sum(PARAMS)) & 0xFF
 *
 * FE-TTLinker: bridge UART full-duplex → half-duplex TTL bidirecional.
 * O TX do ESP32 vai para o barramento; o RX recebe respostas dos servos.
 * Após transmitir, há eco dos bytes enviados no RX. Descartamos o eco
 * lendo-o explicitamente (uart_read_bytes) antes de ler a resposta.
 *
 * Etapa 3.2: WRITE exposto para motion_safety (parking + torque disable).
 */

#include "servo_hal.h"
#include "nb_hw_config_profile.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "hal/usb_serial_jtag_ll.h"

#include <string.h>
#include <stdatomic.h>

/* ── Constantes internas ─────────────────────────────────────────────────── */

#define TAG                  "servo_hal"

#define SCS_HEADER_BYTE      0xFFu
#define SCS_INSTR_PING       0x01u
#define SCS_INSTR_READ       0x02u
#define SCS_INSTR_WRITE      0x03u

/* Tamanho máximo de um pacote de resposta (header + id + len + error + 8 bytes + chk) */
#define SCS_PKT_BUF_SIZE     16u

/* Tempo máximo para o eco chegar no RX após TX completo (ms).
 * A 1Mbps, 6 bytes = 60µs; TTLinker acrescenta uns µs de latência.
 * 5ms é mais do que suficiente. */
#define SCS_ECHO_READ_MS     5u

/* UART RX buffer — suficiente para eco + resposta */
#define UART_RX_BUF_SIZE     256
#define UART_TX_BUF_SIZE     0   /* TX síncrono */

/* Timeout do mutex: maior que um ciclo de motion_task (20ms) + margem */
#define BUS_MUTEX_TIMEOUT_MS 50u

static bool              s_initialized = false;
static SemaphoreHandle_t s_bus_mutex   = NULL;

/* Contadores de erros do bus — para diagnóstico de EMI (F08).
 * Atômicos para leitura segura fora do mutex pelo diagnostics. */
static _Atomic uint32_t s_bus_timeouts = 0;  /* recv_response: timeout aguardando resposta */
static _Atomic uint32_t s_bus_errors   = 0;  /* checksum/header/id inválido */

#define BUS_LOCK()   (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(BUS_MUTEX_TIMEOUT_MS)) == pdTRUE)
#define BUS_UNLOCK() xSemaphoreGive(s_bus_mutex)

/* ── Helpers de protocolo ────────────────────────────────────────────────── */

/*
 * scs_checksum() — Calcula checksum SCSCL.
 * buf aponta para o campo ID (após os dois 0xFF do header).
 * len é o número de bytes de ID até CHECKSUM exclusive.
 */
static uint8_t scs_checksum(const uint8_t *buf, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += buf[i];
    }
    return (uint8_t)(~sum & 0xFFu);
}

/*
 * send_packet() — Monta e envia um pacote de instrução via UART1.
 * Descarta o eco imediatamente após a transmissão.
 *
 * @param id      ID do servo destino.
 * @param instr   Código de instrução (SCS_INSTR_*).
 * @param params  Bytes de parâmetros (pode ser NULL se param_len==0).
 * @param param_len Número de bytes de parâmetro.
 * @return Número de bytes enviados, ou -1 em erro de escrita.
 */
static int send_packet(uint8_t id, uint8_t instr,
                       const uint8_t *params, uint8_t param_len)
{
    /* LEN = INSTR (1) + PARAMS (param_len) + CHECKSUM (1) */
    uint8_t len = (uint8_t)(2u + param_len);

    uint8_t pkt[SCS_PKT_BUF_SIZE];
    pkt[0] = SCS_HEADER_BYTE;
    pkt[1] = SCS_HEADER_BYTE;
    pkt[2] = id;
    pkt[3] = len;
    pkt[4] = instr;
    if (param_len > 0u && params != NULL) {
        memcpy(&pkt[5], params, param_len);
    }
    /* checksum cobre ID, LEN, INSTR, PARAMS */
    pkt[5 + param_len] = scs_checksum(&pkt[2], (size_t)(3u + param_len));

    int total = (int)(6 + param_len);

    int written = uart_write_bytes(NB_SERVO_UART_PORT, (const char *)pkt, (size_t)total);
    if (written != total) {
        ESP_LOGE(TAG, "uart_write_bytes: esperado %d, escreveu %d", total, written);
        return -1;
    }

    /* Descarta eco (FE-TTLinker loopback) com flush após aguardar TX.
     * uart_flush_input é seguro aqui: uart_wait_tx_done garante que TX
     * completou antes do flush, evitando a race com a resposta do servo. */
    uart_wait_tx_done(NB_SERVO_UART_PORT, pdMS_TO_TICKS(10));
    uart_flush_input(NB_SERVO_UART_PORT);

    return written;
}

/*
 * recv_response() — Lê e valida um pacote de resposta do servo.
 *
 * Aguarda os dois bytes de header 0xFF 0xFF, depois lê o restante.
 * Valida checksum e o campo ERROR do pacote.
 *
 * @param expected_id   ID esperado na resposta.
 * @param payload       Buffer para os bytes de payload (após ERROR).
 * @param payload_len   Número de bytes de payload esperados.
 * @param timeout_ms    Timeout total em milissegundos.
 * @return ESP_OK, ESP_ERR_TIMEOUT ou ESP_ERR_INVALID_RESPONSE.
 */
static esp_err_t recv_response(uint8_t expected_id,
                               uint8_t *payload, uint8_t payload_len,
                               uint32_t timeout_ms)
{
    /*
     * Estrutura da resposta:
     *   [0xFF] [0xFF] [ID] [LEN] [ERROR] [PAYLOAD × payload_len] [CHECKSUM]
     * Total: 4 + 1 + payload_len + 1 = 6 + payload_len bytes.
     */
    uint8_t buf[SCS_PKT_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    uint8_t total_expected = (uint8_t)(6u + payload_len);
    if (total_expected > SCS_PKT_BUF_SIZE) {
        ESP_LOGE(TAG, "payload_len=%u excede buffer interno", payload_len);
        return ESP_ERR_INVALID_ARG;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    uint8_t received = 0u;

    while (received < total_expected) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) {
            ESP_LOGW(TAG, "timeout lendo resposta do servo %u (recebidos %u/%u bytes)",
                    expected_id, received, total_expected);
            atomic_fetch_add(&s_bus_timeouts, 1u);
            return ESP_ERR_TIMEOUT;
        }

        TickType_t remaining = deadline - now;
        int n = uart_read_bytes(NB_SERVO_UART_PORT,
                                &buf[received],
                                (uint32_t)(total_expected - received),
                                remaining);
        if (n > 0) {
            received += (uint8_t)n;
        }

        /* Se ainda não chegou o header, re-sincroniza */
        if (received >= 2u && (buf[0] != SCS_HEADER_BYTE || buf[1] != SCS_HEADER_BYTE)) {
            /* Tenta achar 0xFF 0xFF no buffer recebido */
            uint8_t shift = 0;
            for (uint8_t i = 0; i + 1u < received; i++) {
                if (buf[i] == SCS_HEADER_BYTE && buf[i + 1u] == SCS_HEADER_BYTE) {
                    shift = i;
                    break;
                }
            }
            if (shift > 0u) {
                memmove(buf, &buf[shift], (size_t)(received - shift));
                received -= shift;
            } else {
                /* Não encontrou header — descarta tudo e continua aguardando */
                received = 0u;
            }
        }
    }

    /* Valida header */
    if (buf[0] != SCS_HEADER_BYTE || buf[1] != SCS_HEADER_BYTE) {
        ESP_LOGE(TAG, "header inválido: 0x%02X 0x%02X", buf[0], buf[1]);
        atomic_fetch_add(&s_bus_errors, 1u);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Valida ID */
    if (buf[2] != expected_id) {
        ESP_LOGE(TAG, "ID inesperado: esperado %u, recebido %u", expected_id, buf[2]);
        atomic_fetch_add(&s_bus_errors, 1u);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Valida checksum: cobre buf[2..total_expected-2] */
    uint8_t chk_calc = scs_checksum(&buf[2], (size_t)(total_expected - 3u));
    uint8_t chk_recv = buf[total_expected - 1u];
    if (chk_calc != chk_recv) {
        ESP_LOGE(TAG, "checksum inválido: calculado=0x%02X recebido=0x%02X",
                chk_calc, chk_recv);
        atomic_fetch_add(&s_bus_errors, 1u);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Verifica campo ERROR (buf[4]) */
    uint8_t error = buf[4];
    if (error != 0u) {
        ESP_LOGW(TAG, "servo %u reportou erro: 0x%02X", expected_id, error);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Copia payload */
    if (payload_len > 0u && payload != NULL) {
        memcpy(payload, &buf[5], payload_len);
    }

    return ESP_OK;
}

/*
 * attempt_ping() — Uma tentativa de PING sem retry.
 */
static esp_err_t attempt_ping(uint8_t id)
{
    if (send_packet(id, SCS_INSTR_PING, NULL, 0) < 0) {
        return ESP_FAIL;
    }
    /* Resposta de PING: 0xFF 0xFF ID 2 ERROR CHECKSUM — payload_len=0 */
    esp_err_t ret = recv_response(id, NULL, 0, NB_SERVO_TIMEOUT_MS);
    if (ret != ESP_OK) {
        uart_flush_input(NB_SERVO_UART_PORT);
    }
    return ret;
}

/*
 * attempt_read() — Uma tentativa de READ sem retry.
 */
static esp_err_t attempt_read(uint8_t id, uint8_t addr, uint8_t len, uint8_t *buf)
{
    uint8_t params[2] = {addr, len};
    if (send_packet(id, SCS_INSTR_READ, params, 2u) < 0) {
        return ESP_FAIL;
    }
    return recv_response(id, buf, len, NB_SERVO_TIMEOUT_MS);
}

/* ── API pública ─────────────────────────────────────────────────────────── */

esp_err_t servo_hal_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "servo_hal já inicializado");
        return ESP_OK;
    }

    /* GPIO 19/20 são USB D-/D+ no ESP32-S3. O PHY USB hardware fica ativo
     * por padrão após reset e briga com UART1 nesses pinos mesmo sem driver
     * instalado. usb_serial_jtag_ll_phy_enable_pad(false) desconecta o PHY
     * do GPIO matrix; a placa usa CP2102 (UART0) para debug — sem impacto. */
    usb_serial_jtag_ll_phy_enable_pad(false);

    const uart_config_t uart_cfg = {
        .baud_rate  = NB_SERVO_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_param_config(NB_SERVO_UART_PORT, &uart_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config falhou: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_set_pin(NB_SERVO_UART_PORT,
                       NB_SERVO_PIN_TX,
                       NB_SERVO_PIN_RX,
                       UART_PIN_NO_CHANGE,   /* RTS não usado */
                       UART_PIN_NO_CHANGE);  /* CTS não usado */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin falhou: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_driver_install(NB_SERVO_UART_PORT,
                              UART_RX_BUF_SIZE,
                              UART_TX_BUF_SIZE,
                              0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install falhou: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Desabilita GPIO sleep auto-switch para TX e RX do servo.
     * wifi:pm type=1 usa light sleep; sleep_gpio isolate desconecta GPIO 19
     * do UART1 RX nesses microsleeps — gpio_sleep_sel_dis() mantém a config
     * ativa (GPIO matrix → UART1) mesmo durante light sleep. */
    gpio_sleep_sel_dis(NB_SERVO_PIN_TX);
    gpio_sleep_sel_dis(NB_SERVO_PIN_RX);

    s_bus_mutex = xSemaphoreCreateMutex();
    if (!s_bus_mutex) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex falhou");
        uart_driver_delete(NB_SERVO_UART_PORT);
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "UART%d inicializado: TX=GPIO%d RX=GPIO%d %dbps",
            NB_SERVO_UART_PORT, NB_SERVO_PIN_TX, NB_SERVO_PIN_RX,
            NB_SERVO_BAUD_RATE);
    return ESP_OK;
}

esp_err_t servo_hal_ping(uint8_t id)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!BUS_LOCK())    return ESP_ERR_TIMEOUT;

    esp_err_t ret = ESP_ERR_TIMEOUT;
    for (int attempt = 0; attempt < NB_SERVO_RETRY_MAX; attempt++) {
        ret = attempt_ping(id);
        if (ret == ESP_OK) {
            ESP_LOGD(TAG, "PING servo %u: OK (tentativa %d)", id, attempt + 1);
            break;
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PING servo %u: sem resposta após %d tentativas",
                id, NB_SERVO_RETRY_MAX);
    }
    BUS_UNLOCK();
    return ret;
}

esp_err_t servo_hal_read_raw(uint8_t id, uint8_t addr, uint8_t len, uint8_t *buf)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (buf == NULL || len == 0u || len > 8u) return ESP_ERR_INVALID_ARG;
    if (!BUS_LOCK())    return ESP_ERR_TIMEOUT;

    esp_err_t ret = ESP_ERR_TIMEOUT;
    for (int attempt = 0; attempt < NB_SERVO_RETRY_MAX; attempt++) {
        ret = attempt_read(id, addr, len, buf);
        if (ret == ESP_OK) break;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "READ servo %u addr=0x%02X: falhou após %d tentativas",
                id, addr, NB_SERVO_RETRY_MAX);
    }
    BUS_UNLOCK();
    return ret;
}

esp_err_t servo_hal_read_position(uint8_t id, uint16_t *pos)
{
    if (pos == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[2];
    esp_err_t ret = servo_hal_read_raw(id, NB_SERVO_REG_PRESENT_POS_L, 2u, buf);
    if (ret == ESP_OK) {
        /* SCS0009 é big-endian: byte alto no endereço menor.
         * buf[0] = reg[0x38] = H byte, buf[1] = reg[0x39] = L byte.
         * Confirmado via dump de registradores (maio 2026):
         * reg[0x38]=0x02 reg[0x39]=0x7A → posição 634 = 0x027A. */
        *pos = (uint16_t)((uint16_t)buf[0] << 8u | (uint16_t)buf[1]);
    }
    return ret;
}

esp_err_t servo_hal_read_load(uint8_t id, uint16_t *load)
{
    if (load == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[2];
    esp_err_t ret = servo_hal_read_raw(id, NB_SERVO_REG_PRESENT_LOAD_L, 2u, buf);
    if (ret == ESP_OK) {
        /* Big-endian: buf[0] = H byte (reg 0x3C), buf[1] = L byte (reg 0x3D).
         * Bit 10 = direção (0=CCW, 1=CW), bits 9:0 = magnitude. */
        *load = (uint16_t)((uint16_t)buf[0] << 8u | (uint16_t)buf[1]);
    }
    return ret;
}

esp_err_t servo_hal_read_temperature(uint8_t id, uint8_t *temp)
{
    if (temp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return servo_hal_read_raw(id, NB_SERVO_REG_PRESENT_TEMP, 1u, temp);
}

esp_err_t servo_hal_read_voltage(uint8_t id, uint8_t *voltage)
{
    if (voltage == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return servo_hal_read_raw(id, NB_SERVO_REG_PRESENT_VOLTAGE, 1u, voltage);
}

/* ── Registradores de escrita (SCS0009) ──────────────────────────────────── */

#define SCS_REG_TORQUE_ENABLE   0x28u   /* 1=habilitado, 0=livre             */
#define SCS_REG_GOAL_POSITION_L 0x2Au   /* posição alvo low byte             */
#define SCS_REG_GOAL_POSITION_H 0x2Bu   /* posição alvo high byte            */
#define SCS_REG_GOAL_TIME_L     0x2Cu   /* tempo até posição (ms) low byte   */
#define SCS_REG_GOAL_TIME_H     0x2Du   /* tempo até posição (ms) high byte  */

esp_err_t servo_hal_write_position(uint8_t id, uint16_t pos, uint16_t time_ms)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!BUS_LOCK())    return ESP_ERR_TIMEOUT;

    /* SCS0009 big-endian: byte alto no endereço menor.
     * reg[0x2A] = Goal Position H, reg[0x2B] = Goal Position L.
     * reg[0x2C] = Goal Time H,     reg[0x2D] = Goal Time L.
     * Confirmado via dump (maio 2026): reg[0x2A]=0x02 reg[0x2B]=0x7A → pos 634. */
    uint8_t params[5];
    params[0] = SCS_REG_GOAL_POSITION_L;          /* endereço inicial: 0x2A */
    params[1] = (uint8_t)((pos     >> 8u) & 0xFFu); /* reg[0x2A] = pos H      */
    params[2] = (uint8_t)(pos      & 0xFFu);         /* reg[0x2B] = pos L      */
    params[3] = (uint8_t)((time_ms >> 8u) & 0xFFu); /* reg[0x2C] = time H     */
    params[4] = (uint8_t)(time_ms  & 0xFFu);         /* reg[0x2D] = time L     */

    esp_err_t ret = ESP_OK;
    if (send_packet(id, SCS_INSTR_WRITE, params, (uint8_t)sizeof(params)) < 0) {
        ret = ESP_FAIL;
    }
    ESP_LOGD(TAG, "WRITE pos servo %u: pos=%u time=%ums", id, pos, time_ms);
    BUS_UNLOCK();
    return ret;
}

esp_err_t servo_hal_disable_torque(uint8_t id)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!BUS_LOCK())    return ESP_ERR_TIMEOUT;

    uint8_t params[2];
    params[0] = SCS_REG_TORQUE_ENABLE;
    params[1] = 0x00u;

    esp_err_t ret = ESP_OK;
    if (send_packet(id, SCS_INSTR_WRITE, params, (uint8_t)sizeof(params)) < 0) {
        ret = ESP_FAIL;
    } else {
        ESP_LOGI(TAG, "torque DISABLED servo %u", id);
    }
    BUS_UNLOCK();
    return ret;
}

esp_err_t servo_hal_enable_torque(uint8_t id)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!BUS_LOCK())    return ESP_ERR_TIMEOUT;

    uint8_t params[2];
    params[0] = SCS_REG_TORQUE_ENABLE;
    params[1] = 0x01u;

    esp_err_t ret = ESP_OK;
    if (send_packet(id, SCS_INSTR_WRITE, params, (uint8_t)sizeof(params)) < 0) {
        ret = ESP_FAIL;
    } else {
        ESP_LOGI(TAG, "torque ENABLED servo %u", id);
    }
    BUS_UNLOCK();
    return ret;
}

void servo_hal_deinit(void)
{
    if (!s_initialized) return;
    uart_driver_delete(NB_SERVO_UART_PORT);
    if (s_bus_mutex) {
        vSemaphoreDelete(s_bus_mutex);
        s_bus_mutex = NULL;
    }
    s_initialized = false;
    ESP_LOGI(TAG, "UART%d liberado", NB_SERVO_UART_PORT);
}

void servo_hal_get_bus_stats(servo_hal_bus_stats_t *out)
{
    if (out == NULL) return;
    out->timeouts = atomic_load(&s_bus_timeouts);
    out->errors   = atomic_load(&s_bus_errors);
}
