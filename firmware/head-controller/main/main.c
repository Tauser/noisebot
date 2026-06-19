#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nb_inter_mcu_protocol.h"
#include "nb_hw_config_head.h"

static const char *TAG = "nb_head";

void app_main(void)
{
    ESP_LOGI(TAG,
             "head-controller boot protocol=%u.%u reset_reason=%d",
             NB_LINK_PROTOCOL_VERSION_MAJOR,
             NB_LINK_PROTOCOL_VERSION_MINOR,
             (int)esp_reset_reason());
    ESP_LOGI(TAG,
             "target link SPI%d cs=%d sclk=%d mosi=%d miso=%d irq=%d",
             (int)NB_HEAD_LINK_SPI_HOST,
             NB_HEAD_PIN_LINK_CS,
             NB_HEAD_PIN_LINK_SCLK,
             NB_HEAD_PIN_LINK_MOSI,
             NB_HEAD_PIN_LINK_MISO,
             NB_HEAD_PIN_HEAD_IRQ);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
