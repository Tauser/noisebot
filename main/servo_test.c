/*
 * servo_test.c — Diagnóstico de conectividade UART1 / FE-TTLinker
 *
 * TEMPORÁRIO: remover após validação do hardware.
 *
 * Dois testes em sequência:
 *   1. LOOPBACK: confirma que o UART1 TX/RX funciona isolado do TTLinker.
 *      Requer jumper físico entre GPIO 20 (TX) e GPIO 19 (RX).
 *   2. PING: envia PING para servo ID 1 e 2 via TTLinker.
 *      Requer TTLinker ligado e servos energizados.
 */

#include "servo_test.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nb_hw_config.h"

#include <string.h>

static const char *TAG = "servo_test";

/* Escolha um modo por firmware: loopback exige jumper GPIO TX/RX; ping exige
 * TTLinker + servos. Os dois setups físicos não podem coexistir no mesmo boot. */
#define NB_SERVO_TEST_ENABLE_LOOPBACK 0
#define NB_SERVO_TEST_ENABLE_PING     1

/* ── helpers ──────────────────────────────────────────────────────────────── */

static void uart_init_test(void)
{
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

    ESP_LOGI(TAG, "UART%d: TX=GPIO%d RX=GPIO%d @ %dbps",
             NB_SERVO_UART_PORT, NB_SERVO_PIN_TX, NB_SERVO_PIN_RX,
             NB_SERVO_BAUD_RATE);
}

/* ── Teste 1: loopback ────────────────────────────────────────────────────── */

/*
 * Requer jumper entre GPIO 20 (TX) e GPIO 19 (RX).
 * Se passar: UART TX/RX funcionam corretamente.
 * Se falhar: problema de GPIO, driver ou ligação do jumper.
 */
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

    if (len == (int)sizeof(send) &&
        memcmp(send, recv, sizeof(send)) == 0) {
        ESP_LOGI(TAG, "LOOPBACK: OK — UART TX/RX funcionando");
    } else {
        ESP_LOGW(TAG, "LOOPBACK: FALHOU (recebidos %d/%d bytes)", len, (int)sizeof(send));
        for (int i = 0; i < len; i++) {
            ESP_LOGW(TAG, "  recv[%d] = 0x%02X (esperado 0x%02X)", i, recv[i], send[i]);
        }
        ESP_LOGW(TAG, "  -> Verificar jumper entre GPIO%d e GPIO%d",
                 NB_SERVO_PIN_TX, NB_SERVO_PIN_RX);
    }
}
#endif

/* ── Teste 2: ping servo ──────────────────────────────────────────────────── */

static void test_ping_servo(uint8_t id)
{
    uint8_t checksum = (~(id + 0x02u + 0x01u)) & 0xFFu;
    uint8_t ping[]   = {0xFF, 0xFF, id, 0x02, 0x01, checksum};

    uart_flush_input(NB_SERVO_UART_PORT);
    uart_write_bytes(NB_SERVO_UART_PORT, ping, sizeof(ping));
    uart_wait_tx_done(NB_SERVO_UART_PORT, pdMS_TO_TICKS(10));

    /* O TTLinker pode ou não ecoar o pacote transmitido. Lemos a janela toda
     * e procuramos a resposta válida: FF FF ID 02 ERROR CHECKSUM. */
    uint8_t rx[16] = {0};
    int len = uart_read_bytes(NB_SERVO_UART_PORT, rx, sizeof(rx),
                              pdMS_TO_TICKS(50));

    bool ok = false;
    for (int i = 0; i <= len - 6; i++) {
        uint8_t sum = (uint8_t)(rx[i + 2] + rx[i + 3] + rx[i + 4]);
        uint8_t chk = (uint8_t)(~sum & 0xFFu);
        if (rx[i] == 0xFF && rx[i + 1] == 0xFF &&
            rx[i + 2] == id && rx[i + 3] == 0x02 &&
            rx[i + 4] == 0x00 && rx[i + 5] == chk) {
            ok = true;
            break;
        }
    }

    if (ok) {
        ESP_LOGI(TAG, "PING servo %d: OK", id);
    } else {
        ESP_LOGW(TAG, "PING servo %d: sem resposta (len=%d)", id, len);
        for (int i = 0; i < len; i++) {
            ESP_LOGW(TAG, "  byte[%d] = 0x%02X", i, rx[i]);
        }
    }
}

/* ── API pública ──────────────────────────────────────────────────────────── */

void nb_servo_test_ping(void)
{
    uart_init_test();

#if NB_SERVO_TEST_ENABLE_LOOPBACK
    /* Teste 1: loopback — requer jumper GPIO20 <-> GPIO19 */
    test_loopback();
    vTaskDelay(pdMS_TO_TICKS(100));
#endif

#if NB_SERVO_TEST_ENABLE_PING
    /* Teste 2: ping servo — remover jumper, ligar TTLinker */
    ESP_LOGI(TAG, "--- PING SERVO (TTLinker conectado) ---");
    for (uint8_t id = 1; id <= 2; id++) {
        test_ping_servo(id);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
#endif

    uart_driver_delete(NB_SERVO_UART_PORT);
    ESP_LOGI(TAG, "diagnostico concluido");
}
