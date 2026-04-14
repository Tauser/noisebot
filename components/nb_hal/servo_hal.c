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
 * com uart_flush_input() antes de ler a resposta.
 *
 * Etapa 3.2: WRITE exposto para motion_safety (parking + torque disable).
 */

#include "servo_hal.h"
#include "nb_hw_config.h"

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

/* ── Constantes internas ─────────────────────────────────────────────────── */

#define TAG                  "servo_hal"

#define SCS_HEADER_BYTE      0xFFu
#define SCS_INSTR_PING       0x01u
#define SCS_INSTR_READ       0x02u
#define SCS_INSTR_WRITE      0x03u

/* Tamanho máximo de um pacote de resposta (header + id + len + error + 8 bytes + chk) */
#define SCS_PKT_BUF_SIZE     16u

/* Tempo de espera após TX para flush do eco antes de ler resposta (ms) */
#define SCS_ECHO_FLUSH_MS    5u

/* UART RX buffer — suficiente para eco + resposta */
#define UART_RX_BUF_SIZE     256
#define UART_TX_BUF_SIZE     0   /* TX síncrono */

static bool s_initialized = false;

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

    /* Limpa RX antes de enviar para não misturar com eco de chamada anterior */
    uart_flush_input(NB_SERVO_UART_PORT);

    int written = uart_write_bytes(NB_SERVO_UART_PORT, (const char *)pkt, (size_t)total);
    if (written != total) {
        ESP_LOGE(TAG, "uart_write_bytes: esperado %d, escreveu %d", total, written);
        return -1;
    }

    /* Aguarda TX completo e descarta eco (FE-TTLinker loopback) */
    uart_wait_tx_done(NB_SERVO_UART_PORT, pdMS_TO_TICKS(SCS_ECHO_FLUSH_MS));
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
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Valida ID */
    if (buf[2] != expected_id) {
        ESP_LOGE(TAG, "ID inesperado: esperado %u, recebido %u", expected_id, buf[2]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Valida checksum: cobre buf[2..total_expected-2] */
    uint8_t chk_calc = scs_checksum(&buf[2], (size_t)(total_expected - 3u));
    uint8_t chk_recv = buf[total_expected - 1u];
    if (chk_calc != chk_recv) {
        ESP_LOGE(TAG, "checksum inválido: calculado=0x%02X recebido=0x%02X",
                chk_calc, chk_recv);
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
    return recv_response(id, NULL, 0, NB_SERVO_TIMEOUT_MS);
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

    s_initialized = true;
    ESP_LOGI(TAG, "UART%d inicializado: TX=GPIO%d RX=GPIO%d %dbps",
            NB_SERVO_UART_PORT, NB_SERVO_PIN_TX, NB_SERVO_PIN_RX,
            NB_SERVO_BAUD_RATE);
    return ESP_OK;
}

esp_err_t servo_hal_ping(uint8_t id)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_ERR_TIMEOUT;
    for (int attempt = 0; attempt < NB_SERVO_RETRY_MAX; attempt++) {
        ret = attempt_ping(id);
        if (ret == ESP_OK) {
            ESP_LOGD(TAG, "PING servo %u: OK (tentativa %d)", id, attempt + 1);
            return ESP_OK;
        }
        ESP_LOGD(TAG, "PING servo %u: falhou (tentativa %d/%d): %s",
                id, attempt + 1, NB_SERVO_RETRY_MAX, esp_err_to_name(ret));
    }

    ESP_LOGW(TAG, "PING servo %u: sem resposta após %d tentativas",
            id, NB_SERVO_RETRY_MAX);
    return ret;
}

esp_err_t servo_hal_read_raw(uint8_t id, uint8_t addr, uint8_t len, uint8_t *buf)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (buf == NULL || len == 0u || len > 8u) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_ERR_TIMEOUT;
    for (int attempt = 0; attempt < NB_SERVO_RETRY_MAX; attempt++) {
        ret = attempt_read(id, addr, len, buf);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGD(TAG, "READ servo %u addr=0x%02X: falhou (tentativa %d/%d): %s",
                id, addr, attempt + 1, NB_SERVO_RETRY_MAX, esp_err_to_name(ret));
    }

    ESP_LOGW(TAG, "READ servo %u addr=0x%02X: falhou após %d tentativas",
            id, addr, NB_SERVO_RETRY_MAX);
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
        /* Low byte primeiro (little-endian no protocolo SCS) */
        *pos = (uint16_t)((uint16_t)buf[1] << 8u | (uint16_t)buf[0]);
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
        *load = (uint16_t)((uint16_t)buf[1] << 8u | (uint16_t)buf[0]);
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
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Escrita em bloco de 4 bytes a partir de GOAL_POSITION_L (0x2A):
     *   [pos_L, pos_H, time_L, time_H]
     * Formato little-endian (low byte primeiro no protocolo SCS).
     */
    uint8_t params[5];  /* addr(1) + data(4) */
    params[0] = SCS_REG_GOAL_POSITION_L;
    params[1] = (uint8_t)(pos & 0xFFu);
    params[2] = (uint8_t)((pos >> 8u) & 0xFFu);
    params[3] = (uint8_t)(time_ms & 0xFFu);
    params[4] = (uint8_t)((time_ms >> 8u) & 0xFFu);

    /* Fire-and-forget: envia o pacote, não lê resposta.
     * A resposta fica no RX buffer; o próximo send_packet() fará flush. */
    if (send_packet(id, SCS_INSTR_WRITE, params, (uint8_t)sizeof(params)) < 0) {
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "WRITE pos servo %u: pos=%u time=%ums", id, pos, time_ms);
    return ESP_OK;
}

esp_err_t servo_hal_disable_torque(uint8_t id)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t params[2];  /* addr(1) + data(1) */
    params[0] = SCS_REG_TORQUE_ENABLE;
    params[1] = 0x00u;

    if (send_packet(id, SCS_INSTR_WRITE, params, (uint8_t)sizeof(params)) < 0) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "torque DISABLED servo %u", id);
    return ESP_OK;
}

void servo_hal_deinit(void)
{
    if (!s_initialized) {
        return;
    }
    uart_driver_delete(NB_SERVO_UART_PORT);
    s_initialized = false;
    ESP_LOGI(TAG, "UART%d liberado", NB_SERVO_UART_PORT);
}
