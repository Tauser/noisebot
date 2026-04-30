/*
 * servo_test.c — Diagnóstico de conectividade UART1 half-duplex / FE-TTLinker
 *
 * TEMPORÁRIO: remover após validação do hardware.
 * Envia PING para servo ID 1 e ID 2, loga resposta.
 */

#include "servo_test.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nb_hw_config.h"

static const char *TAG = "servo_test";

void nb_servo_test_ping(void)
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

    ESP_LOGI(TAG, "iniciando ping — UART%d GPIO%d half-duplex @ %dbps",
             NB_SERVO_UART_PORT, NB_SERVO_PIN_TX, NB_SERVO_BAUD_RATE);

    for (uint8_t id = 1; id <= 2; id++) {
        uint8_t checksum = (~(id + 0x02u + 0x01u)) & 0xFFu;
        uint8_t ping[]   = {0xFF, 0xFF, id, 0x02, 0x01, checksum};

        uart_write_bytes(NB_SERVO_UART_PORT, ping, sizeof(ping));
        uart_wait_tx_done(NB_SERVO_UART_PORT, pdMS_TO_TICKS(10));
        uart_flush_input(NB_SERVO_UART_PORT);   /* descarta eco half-duplex */

        uint8_t resp[6] = {0};
        int len = uart_read_bytes(NB_SERVO_UART_PORT, resp, sizeof(resp),
                                  pdMS_TO_TICKS(50));

        if (len >= 6 && resp[0] == 0xFF && resp[1] == 0xFF &&
            resp[2] == id  && resp[4] == 0x00) {
            ESP_LOGI(TAG, "SERVO ID %d: OK (erro=0x%02X)", id, resp[4]);
        } else {
            ESP_LOGW(TAG, "SERVO ID %d: sem resposta (len=%d)", id, len);
            for (int i = 0; i < len; i++) {
                ESP_LOGW(TAG, "  byte[%d] = 0x%02X", i, resp[i]);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    uart_driver_delete(NB_SERVO_UART_PORT);
    ESP_LOGI(TAG, "ping concluido");
}
