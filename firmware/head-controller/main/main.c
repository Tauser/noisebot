#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nb_inter_mcu_protocol.h"
#include "nb_hw_config_head.h"
#include "nb_head_status_led.h"
#include "nb_head_link_service.h"
#include "nb_head_display_service.h"
#include "nb_head_camera_service.h"

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

    const esp_err_t led_err = nb_head_status_led_init();
    if (led_err != ESP_OK) {
        ESP_LOGE(TAG, "head status LED init failed: %s", esp_err_to_name(led_err));
    }

    const esp_err_t display_err = nb_head_display_service_init();
    if (display_err != ESP_OK && display_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "head display service init failed: %s",
                 esp_err_to_name(display_err));
    }

    const esp_err_t camera_err =
        nb_head_camera_service_init(nb_head_link_service_send_camera_event);
    if (camera_err != ESP_OK && camera_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "head camera service init failed: %s",
                 esp_err_to_name(camera_err));
    }

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
