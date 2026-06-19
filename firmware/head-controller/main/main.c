#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nb_inter_mcu_protocol.h"
#include "nb_hw_config_head.h"
#include "nb_head_link_service.h"

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

    const esp_err_t link_err = nb_head_link_service_init();
    if (link_err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "dual-MCU link disabled by configuration");
    } else if (link_err != ESP_OK) {
        ESP_LOGE(TAG, "dual-MCU link init failed: %s",
                 esp_err_to_name(link_err));
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
