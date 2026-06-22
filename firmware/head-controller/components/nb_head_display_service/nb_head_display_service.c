#include "nb_head_display_service.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nb_head_display_hal.h"
#include "nb_head_task_config.h"

#ifndef CONFIG_NB_HEAD_DISPLAY_ENABLED
#define CONFIG_NB_HEAD_DISPLAY_ENABLED 0
#endif

static const char *TAG = "nb_head_display";
static nb_head_display_status_t s_status;
static QueueHandle_t s_command_queue;
static StaticQueue_t s_command_queue_storage;
static uint8_t s_command_queue_bytes[sizeof(nb_display_command_t)];
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;

/*
 * DM2.8 -- transicao suave entre expressoes. SET_SCENE nao aplica mais
 * instantaneo: vira o alvo de uma interpolacao de NB_HEAD_DISPLAY_
 * TRANSITION_MS, redesenhada a cada tick (nb_head_display_hal_apply_blend).
 * SET_POWER/FORCE_REFRESH continuam instantaneos via nb_head_display_hal_
 * apply (sem geometria de face envolvida).
 */
#define NB_HEAD_DISPLAY_TICK_MS 20U
#define NB_HEAD_DISPLAY_TRANSITION_MS 220.0f

static void display_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    nb_display_command_t target = {0};
    bool has_target = false;
    uint32_t transition_elapsed_ms = 0U;

    for (;;) {
        nb_display_command_t incoming;
        if (xQueueReceive(s_command_queue, &incoming, 0U) == pdTRUE) {
            taskENTER_CRITICAL(&s_status_lock);
            const bool is_new =
                !s_status.scene_valid ||
                nb_display_generation_is_newer(incoming.generation,
                                               s_status.scene.generation);
            if (!is_new) {
                ++s_status.ignored;
            }
            taskEXIT_CRITICAL(&s_status_lock);

            if (!is_new) {
                ESP_LOGI(TAG, "scene ignored generation=%lu",
                         (unsigned long)incoming.generation);
            } else {
                esp_err_t hw_err = ESP_OK;
                if (incoming.opcode == NB_DISPLAY_OP_SET_SCENE) {
                    if (has_target && s_status.hardware_ready) {
                        /* Finaliza a transicao anterior no alvo antigo antes
                         * de iniciar a proxima -- evita "pular" para um
                         * estado anterior ao alvo interrompido. */
                        (void)nb_head_display_hal_apply_blend(&target, 1.0f);
                    }
                    target = incoming;
                    has_target = true;
                    transition_elapsed_ms = 0U;
                } else if (s_status.hardware_ready) {
                    hw_err = nb_head_display_hal_apply(&incoming);
                }
                if (hw_err != ESP_OK) {
                    taskENTER_CRITICAL(&s_status_lock);
                    ++s_status.hardware_errors;
                    taskEXIT_CRITICAL(&s_status_lock);
                    ESP_LOGE(TAG, "scene hardware apply failed: %s",
                             esp_err_to_name(hw_err));
                }

                taskENTER_CRITICAL(&s_status_lock);
                memcpy(&s_status.scene, &incoming, sizeof(s_status.scene));
                s_status.scene_valid = true;
                ++s_status.accepted;
                taskEXIT_CRITICAL(&s_status_lock);

                ESP_LOGI(TAG,
                         "scene accepted generation=%lu expression=%u "
                         "gaze=%d,%d brightness=%u overlays=0x%04x",
                         (unsigned long)incoming.generation,
                         (unsigned)incoming.expression,
                         (int)incoming.gaze_x_milli,
                         (int)incoming.gaze_y_milli,
                         (unsigned)incoming.brightness,
                         (unsigned)incoming.overlay_flags);
            }
        }

        if (has_target && s_status.hardware_ready) {
            transition_elapsed_ms += NB_HEAD_DISPLAY_TICK_MS;
            const float t = (float)transition_elapsed_ms /
                            NB_HEAD_DISPLAY_TRANSITION_MS;
            const esp_err_t hw_err =
                nb_head_display_hal_apply_blend(&target, t);
            if (hw_err != ESP_OK) {
                taskENTER_CRITICAL(&s_status_lock);
                ++s_status.hardware_errors;
                taskEXIT_CRITICAL(&s_status_lock);
                ESP_LOGE(TAG, "scene blend apply failed: %s",
                         esp_err_to_name(hw_err));
            }
            if (t >= 1.0f) {
                has_target = false;
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(NB_HEAD_DISPLAY_TICK_MS));
    }
}

esp_err_t nb_head_display_service_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.enabled = CONFIG_NB_HEAD_DISPLAY_ENABLED != 0;
    if (!s_status.enabled) {
        ESP_LOGI(TAG, "DM2 display semantic receiver disabled");
        return ESP_ERR_NOT_SUPPORTED;
    }

    const esp_err_t hw_err = nb_head_display_hal_init();
    if (hw_err == ESP_OK) {
        s_status.hardware_ready = true;
        ESP_LOGI(TAG, "DM2 display semantic receiver + ST7789 enabled");
    } else if (hw_err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG,
                 "DM2 display semantic receiver enabled; hardware deferred");
    } else {
        ++s_status.hardware_errors;
        ESP_LOGE(TAG, "ST7789 init failed: %s", esp_err_to_name(hw_err));
        return hw_err;
    }

    s_command_queue = xQueueCreateStatic(
        1U,
        sizeof(nb_display_command_t),
        s_command_queue_bytes,
        &s_command_queue_storage);
    if (s_command_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const BaseType_t created = xTaskCreatePinnedToCore(
        display_task,
        "nb_head_display",
        NB_TASK_HEAD_DISPLAY_STACK,
        NULL,
        NB_TASK_HEAD_DISPLAY_PRIORITY,
        NULL,
        NB_TASK_HEAD_DISPLAY_CORE);
    if (created != pdPASS) {
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t nb_head_display_service_apply(const void *payload, uint16_t length)
{
    if (!s_status.enabled) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!nb_display_command_is_valid(
            (const nb_display_command_t *)payload, length)) {
        ++s_status.rejected;
        return ESP_ERR_INVALID_ARG;
    }

    if (s_command_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const nb_display_command_t *command =
        (const nb_display_command_t *)payload;
    return xQueueOverwrite(s_command_queue, command) == pdPASS
               ? ESP_OK
               : ESP_FAIL;
}

void nb_head_display_service_get_status(nb_head_display_status_t *out)
{
    if (out != NULL) {
        const uint32_t spiram_free = nb_head_display_hal_spiram_free();
        taskENTER_CRITICAL(&s_status_lock);
        s_status.spiram_free_bytes = spiram_free;
        *out = s_status;
        taskEXIT_CRITICAL(&s_status_lock);
    }
}
