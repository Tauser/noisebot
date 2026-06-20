#include "nb_head_camera_service.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#ifndef CONFIG_NB_HEAD_CAMERA_ENABLED
#define CONFIG_NB_HEAD_CAMERA_ENABLED 0
#endif

static const char *TAG = "nb_head_camera";
static nb_head_camera_status_t s_status;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t nb_head_camera_service_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.enabled = CONFIG_NB_HEAD_CAMERA_ENABLED != 0;
    if (!s_status.enabled) {
        ESP_LOGI(TAG, "DM4 camera semantic receiver disabled");
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG,
             "DM4 camera semantic receiver enabled; DVP hardware deferred");
    return ESP_OK;
}

esp_err_t nb_head_camera_service_apply(const void *payload, uint16_t length,
                                       nb_camera_link_event_t *out_event)
{
    if (!s_status.enabled) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const nb_camera_link_command_t *command =
        (const nb_camera_link_command_t *)payload;
    if (!nb_camera_link_command_is_valid(command, length)) {
        taskENTER_CRITICAL(&s_status_lock);
        ++s_status.rejected;
        taskEXIT_CRITICAL(&s_status_lock);
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_status_lock);
    ++s_status.accepted;
    taskEXIT_CRITICAL(&s_status_lock);

    ESP_LOGI(TAG, "command accepted opcode=%u request_id=%lu",
             (unsigned)command->opcode, (unsigned long)command->request_id);

    if (out_event != NULL) {
        memset(out_event, 0, sizeof(*out_event));
        out_event->version = NB_CAMERA_LINK_EVENT_VERSION;
        out_event->status = NB_CAMERA_LINK_STATUS_UNAVAILABLE;
        out_event->mode = command->mode;
        out_event->request_id = command->request_id;
    }
    return ESP_OK;
}

void nb_head_camera_service_get_status(nb_head_camera_status_t *out)
{
    if (out != NULL) {
        taskENTER_CRITICAL(&s_status_lock);
        *out = s_status;
        taskEXIT_CRITICAL(&s_status_lock);
    }
}
