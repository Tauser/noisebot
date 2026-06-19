#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nb_inter_mcu_protocol.h"

static const char *TAG = "nb_head";

void app_main(void)
{
    ESP_LOGI(TAG,
             "head-controller boot protocol=%u.%u reset_reason=%d",
             NB_LINK_PROTOCOL_VERSION_MAJOR,
             NB_LINK_PROTOCOL_VERSION_MINOR,
             (int)esp_reset_reason());

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
