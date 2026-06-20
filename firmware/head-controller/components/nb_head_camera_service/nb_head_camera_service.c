#include "nb_head_camera_service.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nb_head_camera_hal.h"
#include "nb_head_task_config.h"

#ifndef CONFIG_NB_HEAD_CAMERA_ENABLED
#define CONFIG_NB_HEAD_CAMERA_ENABLED 0
#endif

static const char *TAG = "nb_head_camera";
static nb_head_camera_status_t s_status;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static nb_head_camera_event_sink_fn s_sink;
static QueueHandle_t s_command_queue;
static StaticQueue_t s_command_queue_storage;
static uint8_t s_command_queue_bytes[sizeof(nb_camera_link_command_t)];

static void camera_task(void *arg)
{
    (void)arg;
    nb_camera_link_command_t command;

    for (;;) {
        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        nb_camera_link_event_t event;
        memset(&event, 0, sizeof(event));
        event.version = NB_CAMERA_LINK_EVENT_VERSION;
        event.mode = command.mode;
        event.request_id = command.request_id;

        if (!s_status.hardware_ready ||
            command.opcode != NB_CAMERA_LINK_OP_REQUEST_SNAPSHOT) {
            event.status = NB_CAMERA_LINK_STATUS_UNAVAILABLE;
        } else {
            const esp_err_t cap_err = nb_head_camera_hal_capture();
            if (cap_err != ESP_OK) {
                ESP_LOGW(TAG, "captura falhou request_id=%lu: %s",
                         (unsigned long)command.request_id,
                         esp_err_to_name(cap_err));
                event.status = NB_CAMERA_LINK_STATUS_ERROR;
            } else {
                const nb_head_camera_frame_t *frame =
                    nb_head_camera_hal_get_frame();
                if (frame == NULL) {
                    event.status = NB_CAMERA_LINK_STATUS_ERROR;
                } else {
                    event.status = NB_CAMERA_LINK_STATUS_OK;
                    event.width = (uint16_t)frame->width;
                    event.height = (uint16_t)frame->height;
                    event.length = (uint32_t)frame->len;
                    nb_head_camera_hal_release_frame();
                }
            }
        }

        ESP_LOGI(TAG, "request_id=%lu status=%u width=%u height=%u length=%lu",
                 (unsigned long)event.request_id, (unsigned)event.status,
                 (unsigned)event.width, (unsigned)event.height,
                 (unsigned long)event.length);

        if (s_sink != NULL) {
            (void)s_sink(&event);
        }
    }
}

esp_err_t nb_head_camera_service_init(nb_head_camera_event_sink_fn sink)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.enabled = CONFIG_NB_HEAD_CAMERA_ENABLED != 0;
    s_sink = sink;
    if (!s_status.enabled) {
        ESP_LOGI(TAG, "DM4 camera semantic receiver disabled");
        return ESP_ERR_NOT_SUPPORTED;
    }

    const esp_err_t hw_err = nb_head_camera_hal_init();
    if (hw_err == ESP_OK) {
        s_status.hardware_ready = true;
        ESP_LOGI(TAG, "DM4 camera semantic receiver + DVP enabled");
    } else if (hw_err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG,
                 "DM4 camera semantic receiver enabled; DVP hardware deferred");
    } else {
        ESP_LOGE(TAG, "DVP init falhou: %s", esp_err_to_name(hw_err));
        return hw_err;
    }

    s_command_queue = xQueueCreateStatic(
        1U,
        sizeof(nb_camera_link_command_t),
        s_command_queue_bytes,
        &s_command_queue_storage);
    if (s_command_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const BaseType_t created = xTaskCreatePinnedToCore(
        camera_task,
        "nb_head_camera",
        NB_TASK_HEAD_CAMERA_STACK,
        NULL,
        NB_TASK_HEAD_CAMERA_PRIORITY,
        NULL,
        NB_TASK_HEAD_CAMERA_CORE);
    if (created != pdPASS) {
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t nb_head_camera_service_apply(const void *payload, uint16_t length)
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

    if (s_command_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueOverwrite(s_command_queue, command) == pdPASS
               ? ESP_OK
               : ESP_FAIL;
}

void nb_head_camera_service_get_status(nb_head_camera_status_t *out)
{
    if (out != NULL) {
        taskENTER_CRITICAL(&s_status_lock);
        *out = s_status;
        taskEXIT_CRITICAL(&s_status_lock);
    }
}
