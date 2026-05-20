/*
 * servo_test.c — Diagnóstico e validação de movimento dos servos SCS0009
 *
 * TEMPORÁRIO: remover após validação do hardware.
 *
 * Roda antes do boot_manager_run() → antes do WiFi → sem interferência de RF.
 * Modos compiláveis independentes (ajuste as flags abaixo):
 *
 *   LOOPBACK — valida UART TX/RX isolado (jumper GPIO TX ↔ GPIO RX).
 *   PING     — confirma conectividade com cada servo via TTLinker.
 *   MOTION   — sequência de movimento: centro → min → centro → max → centro.
 *              Acompanhe fisicamente: o servo deve se mover de forma suave.
 */

#include "servo_test.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "hal/usb_serial_jtag_ll.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nb_hw_config.h"

#include <string.h>

static const char *TAG = "servo_test";

#define NB_SERVO_TEST_ENABLE_LOOPBACK   0
#define NB_SERVO_TEST_ENABLE_PING       1
#define NB_SERVO_TEST_ENABLE_READ       1   /* lê posição real — confirma escala 0..1023 */
#define NB_SERVO_TEST_ENABLE_BAUD_SWEEP 0
#define NB_SERVO_TEST_ENABLE_SET_P_GAIN 0   /* P=4 já setado via URT-1; EEPROM lock impede write direto */
#define NB_SERVO_TEST_P_GAIN_VALUE      4   /* valor atual nos dois servos (setado via URT-1) */
#define NB_SERVO_TEST_ENABLE_MOTION     1

/* Limites de movimento do teste (em steps, 0–1023) */
#define TEST_CENTER  512
#define TEST_MIN     410   /* ≈ −30° do centro */
#define TEST_MAX     614   /* ≈ +30° do centro */
#define TEST_MOVE_MS 700   /* duração de cada passo em ms */
#define TEST_HOLD_MS 400   /* pausa em cada posição antes do próximo passo */

/* ── UART ─────────────────────────────────────────────────────────────────── */

static void uart_init_test(void)
{
    /* GPIO 19/20 são USB D+/D- no ESP32-S3. O PHY fica ativo por padrão após
     * reset e briga com UART1 nesses pinos. Este teste roda antes do
     * boot_manager_run() (onde servo_hal_init desabilitaria o PHY), então
     * precisamos desabilitar aqui para evitar ruído durante o init do display. */
    usb_serial_jtag_ll_phy_enable_pad(false);

    uart_config_t cfg = {
        .baud_rate  = NB_SERVO_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(NB_SERVO_UART_PORT, &cfg);
    uart_set_pin(NB_SERVO_UART_PORT,
                 NB_SERVO_PIN_TX, NB_SERVO_PIN_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(NB_SERVO_UART_PORT, 256, 256, 0, NULL, 0);

    /* Pull-up interno (~45kΩ) no pino RX para garantir estado idle HIGH
     * e acelerar bordas de subida vindas do TTLinker, que pode ter saída de
     * alta impedância. Um pull-up externo de 10kΩ para 3V3 seria mais forte,
     * mas este elimina componentes extras para o primeiro teste. */
    gpio_pullup_en(NB_SERVO_PIN_RX);

    ESP_LOGI(TAG, "UART%d: TX=GPIO%d RX=GPIO%d @ %dbps (pull-up interno RX ativo)",
             NB_SERVO_UART_PORT, NB_SERVO_PIN_TX, NB_SERVO_PIN_RX,
             NB_SERVO_BAUD_RATE);
}

/* ── Protocolo SCS raw (sem depender do servo_hal) ───────────────────────── */

static uint8_t scs_chk(const uint8_t *buf, int n)
{
    uint32_t s = 0;
    for (int i = 0; i < n; i++) s += buf[i];
    return (uint8_t)(~s & 0xFFu);
}

/*
 * Envia WRITE_DATA para registrador reg, com data_len bytes de dados.
 * Enviado N vezes para compensar perdas de canal (fire-and-forget).
 */
static void scs_write(uint8_t id, uint8_t reg,
                      const uint8_t *data, uint8_t data_len,
                      int times)
{
    /* FF FF ID LEN INSTR REG DATA... CHK */
    uint8_t pkt[16];
    uint8_t len = (uint8_t)(3u + data_len); /* INSTR + REG + DATA + CHK */
    pkt[0] = 0xFF;
    pkt[1] = 0xFF;
    pkt[2] = id;
    pkt[3] = len;
    pkt[4] = 0x03u; /* WRITE_DATA */
    pkt[5] = reg;
    for (int i = 0; i < data_len; i++) pkt[6 + i] = data[i];
    pkt[6 + data_len] = scs_chk(&pkt[2], (int)(4u + data_len));

    int total = (int)(7u + data_len);

    for (int t = 0; t < times; t++) {
        uart_flush_input(NB_SERVO_UART_PORT);
        uart_write_bytes(NB_SERVO_UART_PORT, (const char *)pkt, (size_t)total);
        uart_wait_tx_done(NB_SERVO_UART_PORT, pdMS_TO_TICKS(10));
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* Habilita torque (TORQUE_ENABLE = 0x28, valor 0x01) */
static void scs_torque(uint8_t id, uint8_t enable, int times)
{
    uint8_t d[] = {enable};
    scs_write(id, 0x28u, d, 1u, times);
}

/* Move servo para pos em time_ms milissegundos (GOAL_POSITION = 0x2A–0x2D).
 *
 * IMPORTANTE — big-endian: o SCS0009 armazena valores de 16 bits com o byte
 * alto no endereço menor. As labels "L" (0x2A) e "H" (0x2B) na documentação
 * referem-se a endereço menor/maior, não a byte baixo/alto.
 *
 * Confirmado empiricamente: dump mostrou reg[0x2A]=0x02, reg[0x2B]=0x7A para
 * posição 634 (= 0x027A → H=0x02 em 0x2A, L=0x7A em 0x2B). */
static void scs_move(uint8_t id, uint16_t pos, uint16_t time_ms, int times)
{
    uint8_t d[] = {
        (uint8_t)((pos     >> 8u) & 0xFFu), /* reg[0x2A] = Goal Position H */
        (uint8_t)(pos      & 0xFFu),        /* reg[0x2B] = Goal Position L */
        (uint8_t)((time_ms >> 8u) & 0xFFu), /* reg[0x2C] = Goal Time H     */
        (uint8_t)(time_ms  & 0xFFu),        /* reg[0x2D] = Goal Time L     */
    };
    scs_write(id, 0x2Au, d, 4u, times);
}

/* ── Boot-safe: broadcast TORQUE_DISABLE ─────────────────────────────────── */

/*
 * Enviado SEMPRE ao inicio, antes de qualquer teste.
 * Silencia todos os servos que possam ter ficado com torque ativo de sessões
 * anteriores (URT-1, reset mid-test, etc.). O jitter de um servo com torque
 * ativo gera ruído PWM no barramento que impede qualquer comunicação.
 *
 * Broadcast (ID=0xFE) não gera eco nem resposta — enviamos 15 vezes para
 * garantir entrega mesmo com barramento ruidoso.
 *
 * FF FF FE 04 03 28 00 D2
 *   FE = broadcast, LEN=4, WRITE(03), reg=0x28(TORQUE_ENABLE), val=0x00
 */
static void scs_broadcast_torque_disable(void)
{
    ESP_LOGI(TAG, "--- BROADCAST TORQUE_DISABLE (silenciar bus antes de tudo) ---");
    const uint8_t id  = 0xFEu;
    const uint8_t reg = 0x28u;
    const uint8_t val = 0x00u;
    const uint8_t len = 0x04u;
    uint8_t chk = (uint8_t)(~((uint32_t)id + len + 0x03u + reg + val) & 0xFFu);
    uint8_t pkt[] = {0xFF, 0xFF, id, len, 0x03u, reg, val, chk};

    for (int i = 0; i < 15; i++) {
        uart_write_bytes(NB_SERVO_UART_PORT, pkt, sizeof(pkt));
        uart_wait_tx_done(NB_SERVO_UART_PORT, pdMS_TO_TICKS(10));
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    uart_flush_input(NB_SERVO_UART_PORT);

    /* Aguarda o motor parar de comutar e o ruído EMI desaparecer */
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGI(TAG, "Broadcast concluido — bus deve estar silencioso agora");
}

/* ── Teste 1: loopback ────────────────────────────────────────────────────── */

#if NB_SERVO_TEST_ENABLE_LOOPBACK
static void test_loopback(void)
{
    ESP_LOGI(TAG, "--- LOOPBACK (jumper GPIO%d <-> GPIO%d) ---",
             NB_SERVO_PIN_TX, NB_SERVO_PIN_RX);
    const uint8_t send[] = {0xAA, 0x55, 0x01, 0xFF};
    uart_write_bytes(NB_SERVO_UART_PORT, send, sizeof(send));
    uart_wait_tx_done(NB_SERVO_UART_PORT, pdMS_TO_TICKS(10));
    uint8_t recv[sizeof(send)] = {0};
    int len = uart_read_bytes(NB_SERVO_UART_PORT, recv, sizeof(recv),
                              pdMS_TO_TICKS(20));
    if (len == (int)sizeof(send) && memcmp(send, recv, sizeof(send)) == 0) {
        ESP_LOGI(TAG, "LOOPBACK: OK");
    } else {
        ESP_LOGW(TAG, "LOOPBACK: FALHOU (%d/%d bytes)", len, (int)sizeof(send));
    }
}
#endif

/* ── Teste 2: ping ────────────────────────────────────────────────────────── */

#if NB_SERVO_TEST_ENABLE_PING
/* Escuta o bus por 500ms sem enviar nada — detecta broadcast espontâneo */
static void test_listen_bus(void)
{
    ESP_LOGI(TAG, "--- ESCUTA BUS (500ms, sem TX) ---");
    uart_flush_input(NB_SERVO_UART_PORT);
    uint8_t buf[64] = {0};
    int total = 0;
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(500);
    while (xTaskGetTickCount() < end) {
        int n = uart_read_bytes(NB_SERVO_UART_PORT, buf + total,
                                (uint32_t)(sizeof(buf) - (size_t)total),
                                pdMS_TO_TICKS(20));
        if (n > 0) total += n;
        if (total >= (int)sizeof(buf)) break;
    }
    if (total == 0) {
        ESP_LOGI(TAG, "BUS passivo: silencio total (nenhum byte)");
    } else {
        ESP_LOGI(TAG, "BUS passivo: %d bytes recebidos (servo ativo no bus!)", total);
        for (int i = 0; i < total; i++) {
            ESP_LOGI(TAG, "  [%02d] = 0x%02X", i, buf[i]);
        }
    }
}

/*
 * Envia PING e lê TODO o tráfego do bus numa janela única.
 * O servo pode estar mandando broadcast contínuo — não separamos eco de resposta:
 * lemos tudo e procuramos o padrão FF FF em qualquer posição.
 */
static bool test_ping_servo(uint8_t id)
{
    ESP_LOGI(TAG, "--- PING id=%d ---", id);
    uint8_t checksum = (~(id + 0x02u + 0x01u)) & 0xFFu;
    uint8_t ping[]   = {0xFF, 0xFF, id, 0x02, 0x01, checksum};

    uart_flush_input(NB_SERVO_UART_PORT);
    uart_write_bytes(NB_SERVO_UART_PORT, ping, sizeof(ping));
    uart_wait_tx_done(NB_SERVO_UART_PORT, pdMS_TO_TICKS(10));

    /* Espera TX(60µs) + delay do servo(500µs) + RX(60µs) + margem generosa */
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Lê tudo de uma vez: eco + resposta + broadcasts do servo */
    uint8_t buf[128] = {0};
    int len = uart_read_bytes(NB_SERVO_UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "PING id=%d: %d bytes no total", id, len);
    for (int j = 0; j < len; j++) {
        ESP_LOGI(TAG, "  [%02d] = 0x%02X", j, buf[j]);
    }

    /* Procura qualquer par FF FF e analisa o pacote */
    bool found_ping_resp = false;
    for (int i = 0; i <= len - 6; i++) {
        if (buf[i] != 0xFF || buf[i+1] != 0xFF) continue;
        uint8_t pid  = buf[i+2];
        uint8_t plen = buf[i+3];
        uint8_t perr = buf[i+4];
        uint8_t pchk = buf[i+5];
        uint8_t expected = (uint8_t)(~((uint32_t)pid + plen + perr) & 0xFFu);
        ESP_LOGI(TAG, "  [FF FF] pos=%d ID=%d LEN=%d ERR=0x%02X CHK=0x%02X (exp=0x%02X)%s",
                 i, pid, plen, perr, pchk, expected,
                 (pchk == expected) ? " CHK-OK" : "");
        if (plen == 0x02u && perr == 0x00u && pchk == expected) {
            ESP_LOGI(TAG, "  *** PING OK: servo ID=%d respondeu (pos=%d) ***", pid, i);
            found_ping_resp = true;
        }
    }
    if (!found_ping_resp) {
        ESP_LOGW(TAG, "PING servo %d: sem resposta valida de PING", id);
    }
    return found_ping_resp;
}

/*
 * Varre IDs 1..max_id silenciosamente.
 * Usa a mesma janela única de leitura — lida com broadcast contínuo.
 * Loga apenas IDs onde encontrou resposta de PING válida.
 */
static int test_scan_ids(uint8_t max_id)
{
    ESP_LOGI(TAG, "--- SCAN IDs 1..%d ---", max_id);
    int found = 0;
    for (uint8_t id = 1; id <= max_id; id++) {
        uint8_t checksum = (~(id + 0x02u + 0x01u)) & 0xFFu;
        uint8_t ping[]   = {0xFF, 0xFF, id, 0x02, 0x01, checksum};

        uart_flush_input(NB_SERVO_UART_PORT);
        uart_write_bytes(NB_SERVO_UART_PORT, ping, sizeof(ping));
        uart_wait_tx_done(NB_SERVO_UART_PORT, pdMS_TO_TICKS(10));
        vTaskDelay(pdMS_TO_TICKS(10));

        uint8_t buf[64] = {0};
        int len = uart_read_bytes(NB_SERVO_UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(100));

        for (int i = 0; i <= len - 6; i++) {
            if (buf[i] != 0xFF || buf[i+1] != 0xFF) continue;
            uint8_t pid  = buf[i+2];
            uint8_t plen = buf[i+3];
            uint8_t perr = buf[i+4];
            uint8_t pchk = buf[i+5];
            uint8_t exp  = (uint8_t)(~((uint32_t)pid + plen + perr) & 0xFFu);
            if (plen == 0x02u && perr == 0x00u && pchk == exp) {
                ESP_LOGI(TAG, "  *** SCAN: servo ID=%d encontrado (perguntei ID=%d) ***",
                         pid, id);
                found++;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (found == 0) {
        ESP_LOGW(TAG, "SCAN: nenhum servo respondeu nos IDs 1..%d", max_id);
    } else {
        ESP_LOGI(TAG, "SCAN: %d servo(s) encontrado(s)", found);
    }
    return found;
}
#endif

/* ── Teste 2b: baud rate sweep ────────────────────────────────────────────── */

#if NB_SERVO_TEST_ENABLE_BAUD_SWEEP

/*
 * Reinicializa o driver UART com um novo baud rate sem tocar nos pinos.
 * Chamado pelo baud sweep para testar cada taxa. Reaplica pull-up no RX.
 */
static void uart_reinit_baud(int baud)
{
    uart_driver_delete(NB_SERVO_UART_PORT);
    uart_config_t cfg = {
        .baud_rate  = baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(NB_SERVO_UART_PORT, &cfg);
    uart_set_pin(NB_SERVO_UART_PORT,
                 NB_SERVO_PIN_TX, NB_SERVO_PIN_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(NB_SERVO_UART_PORT, 256, 256, 0, NULL, 0);
    gpio_pullup_en(NB_SERVO_PIN_RX);
}

/*
 * Tenta cada baud rate suportado pelo SCS0009 e procura resposta de PING.
 *
 * Motivação: 9V pode ter corrompido a EEPROM do servo → baud rate alterado
 * após power cycle. Quando o UART está em 1Mbps e o servo fala outra taxa,
 * o receptor vê apenas framing errors (descartados) → bus_listen = silêncio.
 *
 * Para cada taxa: escuta 200ms passivamente (detecta broadcast do servo)
 * e depois envia PING para IDs 1 e 2, procurando resposta válida.
 *
 * Retorna o baud rate funcional, ou 0 se nenhum respondeu.
 * Ao retornar com sucesso, o UART já está configurado naquele baud rate.
 * Ao retornar 0, o UART fica na última taxa testada (38400).
 */
static int test_baud_sweep(void)
{
    static const int k_baud[] = {
        1000000, 500000, 250000, 128000, 115200, 76800, 57600, 38400
    };
    static const int k_nbaud = (int)(sizeof(k_baud) / sizeof(k_baud[0]));

    ESP_LOGI(TAG, "--- BAUD SWEEP (%d taxas, IDs 1-2) ---", k_nbaud);

    for (int b = 0; b < k_nbaud; b++) {
        int baud = k_baud[b];
        uart_reinit_baud(baud);
        vTaskDelay(pdMS_TO_TICKS(30));

        /* Escuta passiva: servo vivo neste baud emitiria broadcast */
        uart_flush_input(NB_SERVO_UART_PORT);
        uint8_t lbuf[32] = {0};
        int llen = uart_read_bytes(NB_SERVO_UART_PORT, lbuf,
                                   sizeof(lbuf), pdMS_TO_TICKS(200));
        if (llen > 0) {
            ESP_LOGI(TAG, "  [%7d bps] passivo: %d bytes (servo ativo!)",
                     baud, llen);
            for (int i = 0; i < llen; i++) {
                ESP_LOGI(TAG, "    [%02d] = 0x%02X", i, lbuf[i]);
            }
        }

        /* PING para IDs 1 e 2 */
        for (uint8_t id = 1u; id <= 2u; id++) {
            uint8_t chk    = (~(id + 0x02u + 0x01u)) & 0xFFu;
            uint8_t ping[] = {0xFF, 0xFF, id, 0x02, 0x01, chk};

            uart_flush_input(NB_SERVO_UART_PORT);
            uart_write_bytes(NB_SERVO_UART_PORT, ping, sizeof(ping));
            uart_wait_tx_done(NB_SERVO_UART_PORT, pdMS_TO_TICKS(20));
            vTaskDelay(pdMS_TO_TICKS(20));

            uint8_t buf[32] = {0};
            int len = uart_read_bytes(NB_SERVO_UART_PORT, buf,
                                      sizeof(buf), pdMS_TO_TICKS(150));

            for (int i = 0; i <= len - 6; i++) {
                if (buf[i] != 0xFF || buf[i+1] != 0xFF) continue;
                uint8_t pid  = buf[i+2];
                uint8_t plen = buf[i+3];
                uint8_t perr = buf[i+4];
                uint8_t pchk = buf[i+5];
                uint8_t exp  = (uint8_t)(~((uint32_t)pid + plen + perr) & 0xFFu);
                if (plen == 0x02u && perr == 0x00u && pchk == exp) {
                    ESP_LOGI(TAG,
                             "  *** BAUD SWEEP OK: servo ID=%d @ %d bps ***",
                             pid, baud);
                    return baud;
                }
            }
        }

        if (llen == 0) {
            ESP_LOGI(TAG, "  [%7d bps] silencio — sem resposta", baud);
        }
    }

    ESP_LOGW(TAG, "BAUD SWEEP: servo nao respondeu em nenhuma das %d taxas",
             k_nbaud);
    return 0;
}

#endif /* NB_SERVO_TEST_ENABLE_BAUD_SWEEP */

/* ── Teste 2c: leitura de posição atual ───────────────────────────────────── */

#if NB_SERVO_TEST_ENABLE_READ

/*
 * Envia READ_DATA para o registrador de posição atual (0x38, 2 bytes).
 *
 * Serve para dois propósitos:
 *   1. Determinar a escala do servo: resposta ≤1023 → 0..1023 (SCS0009 10-bit);
 *                                    resposta >1023 → 0..4095 (STS series 12-bit).
 *   2. Conhecer a posição real antes de enviar qualquer movimento.
 *
 * Protocolo SCS READ:
 *   TX:       FF FF [ID] 04 02 38 02 [CHK]
 *   Eco:      FF FF [ID] 04 02 38 02 [CHK]            ← instrução=0x02 em pos[4], descartado
 *   Resposta: FF FF [ID] 04 00 [POS_L] [POS_H] [CHK]  ← ERR=0x00 em pos[4]
 */
static bool test_read_position(uint8_t id, uint16_t *out_pos)
{
    ESP_LOGI(TAG, "--- READ POSITION id=%d ---", id);
    uint8_t chk = (uint8_t)(~((uint32_t)id + 0x04u + 0x02u + 0x38u + 0x02u) & 0xFFu);
    uint8_t pkt[] = {0xFF, 0xFF, id, 0x04, 0x02, 0x38, 0x02, chk};

    uart_flush_input(NB_SERVO_UART_PORT);
    uart_write_bytes(NB_SERVO_UART_PORT, pkt, sizeof(pkt));
    uart_wait_tx_done(NB_SERVO_UART_PORT, pdMS_TO_TICKS(10));
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Eco (8 bytes) + resposta (8 bytes) = 16 bytes esperados */
    uint8_t buf[32] = {0};
    int len = uart_read_bytes(NB_SERVO_UART_PORT, buf, sizeof(buf),
                              pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "READ id=%d: %d bytes recebidos", id, len);
    for (int i = 0; i < len; i++) {
        ESP_LOGI(TAG, "  [%02d] = 0x%02X", i, buf[i]);
    }

    /* Procura FF FF com LEN=4 e ERR=0x00 (descarta eco onde ERR=0x02=instrução) */
    for (int i = 0; i <= len - 8; i++) {
        if (buf[i] != 0xFF || buf[i+1] != 0xFF) continue;
        uint8_t rid  = buf[i+2];
        uint8_t rlen = buf[i+3];
        uint8_t rerr = buf[i+4];
        /* SCS0009 big-endian: reg[0x38] = byte alto, reg[0x39] = byte baixo.
         * Confirmado no dump: reg[0x38]=0x02 reg[0x39]=0x7A → posição 634.
         * Variáveis nomeadas pelo papel real, não pelo label do datasheet. */
        uint8_t byte_h = buf[i+5]; /* conteúdo de reg[0x38] — byte ALTO */
        uint8_t byte_l = buf[i+6]; /* conteúdo de reg[0x39] — byte BAIXO */
        uint8_t rchk   = buf[i+7];
        if (rlen != 0x04u || rerr != 0x00u) continue;
        uint8_t exp = (uint8_t)(~((uint32_t)rid + rlen + rerr + byte_h + byte_l) & 0xFFu);
        if (rchk != exp) continue;
        uint16_t pos = ((uint16_t)byte_h << 8u) | (uint16_t)byte_l;
        ESP_LOGI(TAG, "  *** READ OK: ID=%d posicao=%u (0x%03X) [H=0x%02X L=0x%02X] — escala: %s ***",
                 rid, pos, pos, byte_h, byte_l,
                 (pos <= 1023u) ? "0..1023 OK" : ">1023 ERRO endian?");
        if (out_pos) *out_pos = pos;
        return true;
    }
    ESP_LOGW(TAG, "READ POSITION id=%d: sem resposta valida", id);
    return false;
}

static void test_read_all_positions(void)
{
    ESP_LOGI(TAG, "=== READ POSICAO ATUAL (servo parado, sem movimento) ===");
    uint16_t pos1 = 0, pos2 = 0;
    bool ok1 = test_read_position(1, &pos1);
    vTaskDelay(pdMS_TO_TICKS(30));
    bool ok2 = test_read_position(2, &pos2);
    if (ok1 || ok2) {
        ESP_LOGI(TAG, "Resumo: ID1=%u  ID2=%u", pos1, pos2);
        if (pos1 <= 1023u && pos2 <= 1023u) {
            ESP_LOGI(TAG, "Escala CONFIRMADA: 0..1023 — TEST_CENTER=512 esta correto");
        } else {
            ESP_LOGW(TAG, "Escala: valor >1023 detectado — verificar se servo e 0..4095");
            ESP_LOGW(TAG, "Se 0..4095: centro correto = 2048, TEST_CENTER/MIN/MAX incorretos");
        }
    }
}

/*
 * Lê um bloco contíguo de registradores e imprime byte a byte com endereço.
 * Usado para localizar onde a posição real está quando READ_POSITION retorna 0
 * mas o software oficial mostra valor diferente.
 * Procura automaticamente pares little-endian que correspondam a ~520 (0x0208).
 */
static void test_register_dump(uint8_t id, uint8_t start_reg, uint8_t reg_len)
{
    ESP_LOGI(TAG, "--- REG DUMP id=%d reg=0x%02X..0x%02X (len=%d) ---",
             id, start_reg, (uint8_t)(start_reg + reg_len - 1u), reg_len);

    uint8_t chk = (uint8_t)(~((uint32_t)id + 0x04u + 0x02u + start_reg + reg_len) & 0xFFu);
    uint8_t pkt[] = {0xFF, 0xFF, id, 0x04, 0x02, start_reg, reg_len, chk};

    uart_flush_input(NB_SERVO_UART_PORT);
    uart_write_bytes(NB_SERVO_UART_PORT, pkt, sizeof(pkt));
    uart_wait_tx_done(NB_SERVO_UART_PORT, pdMS_TO_TICKS(10));
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Echo (8 bytes) + resposta (6 + reg_len bytes) */
    uint8_t buf[96] = {0};
    int len = uart_read_bytes(NB_SERVO_UART_PORT, buf,
                              (uint32_t)sizeof(buf), pdMS_TO_TICKS(300));

    ESP_LOGI(TAG, "  raw: %d bytes recebidos", len);

    /* Procura resposta: FF FF [ID] [LEN=reg_len+2] [ERR=00] [DATA...] [CHK] */
    for (int i = 0; i <= len - (int)(6 + reg_len); i++) {
        if (buf[i] != 0xFF || buf[i+1] != 0xFF) continue;
        uint8_t rid  = buf[i+2];
        uint8_t rlen = buf[i+3];
        uint8_t rerr = buf[i+4];
        if (rerr != 0x00u) continue;
        if (rlen != (uint8_t)(reg_len + 2u)) continue;

        /* Verifica checksum */
        uint32_t sum = (uint32_t)rid + rlen + rerr;
        for (int j = 0; j < (int)reg_len; j++) sum += buf[i+5+j];
        uint8_t exp  = (uint8_t)(~sum & 0xFFu);
        uint8_t rchk = buf[i+5+reg_len];
        if (rchk != exp) {
            ESP_LOGW(TAG, "  CHK errado: recebido=0x%02X esperado=0x%02X (pos=%d)",
                     rchk, exp, i);
            continue;
        }

        /* Imprime registradores com endereço */
        ESP_LOGI(TAG, "  === DUMP REGISTRADORES (CHK OK) ===");
        for (int j = 0; j < (int)reg_len; j++) {
            uint8_t addr = (uint8_t)(start_reg + j);
            uint8_t val  = buf[i+5+j];
            ESP_LOGI(TAG, "  reg[0x%02X/%3d] = 0x%02X (%3d)", addr, addr, val, val);
        }

        /* Interpreta todos os pares little-endian uint16 e procura valores 400..700 */
        ESP_LOGI(TAG, "  --- pares uint16 little-endian (procura 400..700) ---");
        for (int j = 0; j < (int)reg_len - 1; j++) {
            uint16_t v = (uint16_t)buf[i+5+j] | ((uint16_t)buf[i+5+j+1] << 8u);
            if (v >= 400u && v <= 700u) {
                ESP_LOGI(TAG, "  *** reg[0x%02X+0x%02X] = %u  ← candidato a posicao ***",
                         (uint8_t)(start_reg + j),
                         (uint8_t)(start_reg + j + 1), v);
            }
        }
        return;
    }
    ESP_LOGW(TAG, "  sem resposta valida (len=%d)", len);
}

#endif /* NB_SERVO_TEST_ENABLE_READ */

/* ── Teste 2d: ajuste de P gain (EEPROM 0x15) ────────────────────────────── */

#if NB_SERVO_TEST_ENABLE_SET_P_GAIN
/*
 * Escreve o ganho proporcional do PID de posição na EEPROM do servo.
 *
 * EEPROM 0x15 = P Proportionality Coefficient (default=15, range 0–254).
 * P alto + servo sem carga mecânica = PID oscila em volta do setpoint (jitter).
 * Reduzir P diminui a oscilação em idle às custas de resposta mais lenta.
 *
 * Escrita em EEPROM é persistente (survives power cycle). Escrita síncrona,
 * portanto chamada apenas no setup — nunca em runtime.
 *
 * Nota: registro 0x30 = 0x01 nos dumps sugere EEPROM Lock ativo. Se o servo
 * ignorar este write, será necessário desbloquear primeiro (write 0x00 em 0x30,
 * write P, write 0x01 em 0x30). Por ora tentamos escrita direta.
 */
static void test_set_p_gain(uint8_t id, uint8_t p_value)
{
    ESP_LOGI(TAG, "--- SET P GAIN id=%d val=%d (EEPROM 0x15, default=15) ---",
             id, p_value);
    uint8_t d[] = {p_value};
    /* Torque deve estar desabilitado para escrever EEPROM (SCS0009 recomenda) */
    scs_torque(id, 0x00u, 3);
    vTaskDelay(pdMS_TO_TICKS(20));
    scs_write(id, 0x15u, d, 1u, 3);
    /* EEPROM write time: ~15ms típico; 100ms com margem generosa */
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "  P gain escrito — verificar com dump se necessario");
}
#endif

/* ── Teste 3: sequência de movimento ─────────────────────────────────────── */

#if NB_SERVO_TEST_ENABLE_MOTION
static void test_motion_servo(uint8_t id, const char *name)
{
    ESP_LOGI(TAG, "--- MOTION servo %d (%s) ---", id, name);
    ESP_LOGI(TAG, "Observe o servo fisicamente durante o teste.");

    /* Habilita torque (8 envios para garantir entrega com RF) */
    ESP_LOGI(TAG, "[%s] habilitando torque...", name);
    scs_torque(id, 0x01u, 8);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Centro */
    ESP_LOGI(TAG, "[%s] → CENTRO (%d steps)", name, TEST_CENTER);
    scs_move(id, TEST_CENTER, TEST_MOVE_MS, 5);
    vTaskDelay(pdMS_TO_TICKS(TEST_MOVE_MS + TEST_HOLD_MS));

    /* Mínimo */
    ESP_LOGI(TAG, "[%s] → MIN (%d steps, ≈−30°)", name, TEST_MIN);
    scs_move(id, TEST_MIN, TEST_MOVE_MS, 5);
    vTaskDelay(pdMS_TO_TICKS(TEST_MOVE_MS + TEST_HOLD_MS));

    /* Volta ao centro */
    ESP_LOGI(TAG, "[%s] → CENTRO (%d steps)", name, TEST_CENTER);
    scs_move(id, TEST_CENTER, TEST_MOVE_MS, 5);
    vTaskDelay(pdMS_TO_TICKS(TEST_MOVE_MS + TEST_HOLD_MS));

    /* Máximo */
    ESP_LOGI(TAG, "[%s] → MAX (%d steps, ≈+30°)", name, TEST_MAX);
    scs_move(id, TEST_MAX, TEST_MOVE_MS, 5);
    vTaskDelay(pdMS_TO_TICKS(TEST_MOVE_MS + TEST_HOLD_MS));

    /* Volta ao centro e fica */
    ESP_LOGI(TAG, "[%s] → CENTRO (%d steps) — fim", name, TEST_CENTER);
    scs_move(id, TEST_CENTER, TEST_MOVE_MS, 5);
    vTaskDelay(pdMS_TO_TICKS(TEST_MOVE_MS + TEST_HOLD_MS));

    /* Lê posição antes de soltar o torque — prova o pipeline end-to-end.
     * Esperado: ~512 (centro). Tolerância ±20 steps (≈ ±6°). */
    {
        uint16_t pos = 0;
        bool ok = test_read_position(id, &pos);
        if (ok) {
            int delta = (int)pos - 512;
            if (delta < 0) delta = -delta;
            if (delta <= 20) {
                ESP_LOGI(TAG, "[%s] POSICAO FINAL: %u steps — dentro do centro (±20) ✓", name, pos);
            } else {
                ESP_LOGW(TAG, "[%s] POSICAO FINAL: %u steps — desvio de %d steps do centro (512)", name, pos, (int)pos - 512);
            }
        } else {
            ESP_LOGW(TAG, "[%s] POSICAO FINAL: falha na leitura", name);
        }
    }

    /* Desabilita torque: servo fica livre (não vibra segurando posição) */
    ESP_LOGI(TAG, "[%s] desabilitando torque", name);
    scs_torque(id, 0x00u, 3);

    ESP_LOGI(TAG, "[%s] teste concluido — servo deve estar no centro", name);
}
#endif

/* ── API pública ──────────────────────────────────────────────────────────── */

void nb_servo_test_ping(void)
{
    uart_init_test();

    /* Sempre: silencia todos os servos antes de qualquer teste.
     * Previne ruído EMI de motor em jitter que corrompe o barramento. */
    scs_broadcast_torque_disable();

#if NB_SERVO_TEST_ENABLE_LOOPBACK
    test_loopback();
    vTaskDelay(pdMS_TO_TICKS(100));
#endif

#if NB_SERVO_TEST_ENABLE_PING
    /* Primeiro escuta passiva: confirma se há atividade no bus */
    test_listen_bus();

    /* PING verbose para IDs esperados (1 e 2) */
    ESP_LOGI(TAG, "--- PING SERVO IDs 1 e 2 ---");
    bool s1 = test_ping_servo(1);
    vTaskDelay(pdMS_TO_TICKS(50));
    bool s2 = test_ping_servo(2);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Scan silencioso: encontra qualquer servo mesmo com ID diferente */
    if (!s1 && !s2) {
        vTaskDelay(pdMS_TO_TICKS(100));
        test_scan_ids(20);
    }

#if NB_SERVO_TEST_ENABLE_READ
    /* Lê posição atual de ambos os servos sem enviar nenhum movimento.
     * Confirma a escala (0..1023 vs 0..4095) e a posição real em repouso. */
    if (s1 || s2) {
        vTaskDelay(pdMS_TO_TICKS(50));
        test_read_all_positions();

        /* Dump completo dos registradores RAM para localizar onde a posição
         * real está quando READ 0x38 retorna 0 mas o URT-1 mostra ~520.
         *
         * Bloco 1: 0x28..0x47 (len=32) — inclui Torque Enable, Goal Position,
         *          Goal Time, Goal Speed e Present Position (0x38).
         * Bloco 2: 0x38..0x47 (len=16) — zoom no bloco de estado presente:
         *          Present Position, Present Speed, Present Load,
         *          Present Voltage, Present Temperature.
         *
         * Procura automaticamente pares LE no range 400..700 (posição ~520). */
        if (s1) {
            vTaskDelay(pdMS_TO_TICKS(50));
            test_register_dump(1, 0x28u, 32u);
            vTaskDelay(pdMS_TO_TICKS(50));
            test_register_dump(1, 0x38u, 16u);
        }
        if (s2) {
            vTaskDelay(pdMS_TO_TICKS(50));
            test_register_dump(2, 0x28u, 32u);
            vTaskDelay(pdMS_TO_TICKS(50));
            test_register_dump(2, 0x38u, 16u);
        }
    }
#endif

#if NB_SERVO_TEST_ENABLE_BAUD_SWEEP
    /* Sweep de baud rates: diagnóstico de EEPROM corrompida pelo 9V.
     * Se o servo não respondeu em 1Mbps, testa as outras 7 taxas SCS. */
    if (!s1 && !s2) {
        vTaskDelay(pdMS_TO_TICKS(100));
        int found_baud = test_baud_sweep();
        if (found_baud > 0 && found_baud != NB_SERVO_BAUD_RATE) {
            /* Servo vivo mas em baud rate diferente — confirma IDs */
            ESP_LOGW(TAG,
                     "ATENCAO: servo responde @ %d bps, nao @ %d bps (1Mbps).",
                     found_baud, NB_SERVO_BAUD_RATE);
            ESP_LOGW(TAG,
                     "EEPROM provavelmente corrompida. Reconfigurar baud rate.");
            s1 = test_ping_servo(1);
            vTaskDelay(pdMS_TO_TICKS(30));
            s2 = test_ping_servo(2);
        } else if (found_baud == 0) {
            /* Nenhuma taxa funcionou — servo provavelmente sem resposta */
            ESP_LOGW(TAG,
                     "BAUD SWEEP: nenhuma taxa funcionou — verifique alimentacao"
                     " 5V no conector do servo e continuidade do cabo SCS.");
        }
    }
#endif /* NB_SERVO_TEST_ENABLE_BAUD_SWEEP */

#else
    bool s1 = true, s2 = true; /* assume conectado se ping desabilitado */
#endif

#if NB_SERVO_TEST_ENABLE_SET_P_GAIN
    /* Reduz P gain antes do MOTION para eliminar oscilação PID sem carga.
     * Escrito na EEPROM — persiste após power cycle. */
    if (s1) test_set_p_gain(1, NB_SERVO_TEST_P_GAIN_VALUE);
    if (s2) test_set_p_gain(2, NB_SERVO_TEST_P_GAIN_VALUE);
    vTaskDelay(pdMS_TO_TICKS(100));
#endif

#if NB_SERVO_TEST_ENABLE_MOTION
    /* Só testa movimento se pelo menos um servo respondeu ao ping */
    if (s1 || s2) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (s1) test_motion_servo(1, "PAN");
        vTaskDelay(pdMS_TO_TICKS(300));
        if (s2) test_motion_servo(2, "TILT");
    } else {
        ESP_LOGW(TAG, "MOTION: pulado — nenhum servo respondeu ao ping");
    }
#endif

    uart_driver_delete(NB_SERVO_UART_PORT);
    ESP_LOGI(TAG, "=== diagnostico concluido ===");
}
