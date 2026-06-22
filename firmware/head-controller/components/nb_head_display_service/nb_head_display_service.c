#include "nb_head_display_service.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
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
static uint32_t s_status_icon_bits;
static bool s_status_icon_bits_dirty;
static TaskHandle_t s_display_task_handle;

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
    nb_display_command_t target = {0};
    bool has_target = false;
    int64_t transition_started_us = 0;

    for (;;) {
        const TickType_t cycle_started = xTaskGetTickCount();
        bool force_redraw = false;

        taskENTER_CRITICAL(&s_status_lock);
        const uint32_t icon_bits = s_status_icon_bits;
        const bool icon_bits_changed = s_status_icon_bits_dirty;
        s_status_icon_bits_dirty = false;
        taskEXIT_CRITICAL(&s_status_lock);
        if (icon_bits_changed) {
            force_redraw = true;
        }

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
                    const bool expression_changed =
                        !has_target ||
                        incoming.expression != target.expression;
                    if (has_target && expression_changed &&
                        s_status.hardware_ready) {
                        /* Finaliza a transicao anterior no alvo antigo antes
                         * de iniciar a proxima -- evita "pular" para um
                         * estado anterior ao alvo interrompido. */
                        (void)nb_head_display_hal_apply_blend(
                            &target, 1.0f, true, icon_bits);
                    }
                    target = incoming;
                    has_target = true;
                    if (expression_changed) {
                        transition_started_us = esp_timer_get_time();
                    } else {
                        /*
                         * Gaze, overlay e brilho nao reiniciam a interpolacao
                         * facial. Se ela ja terminou, basta redesenhar uma vez
                         * com os novos valores. Reiniciar 220 ms a cada gaze
                         * mantinha pushSprite() continuo e impedia IDLE1 de
                         * alimentar o task watchdog.
                         */
                        force_redraw = true;
                    }
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
            /* Continua redesenhando indefinidamente apos a transicao
             * terminar (t satura em 1.0) -- o blink autonomo do head
             * (DM2.9) precisa de redesenho continuo, nao so durante a
             * troca de expressao. */
            const int64_t elapsed_us =
                esp_timer_get_time() - transition_started_us;
            const float t =
                (float)elapsed_us /
                (NB_HEAD_DISPLAY_TRANSITION_MS * 1000.0f);
            const esp_err_t hw_err = nb_head_display_hal_apply_blend(
                &target, t, force_redraw, icon_bits);
            if (hw_err != ESP_OK) {
                taskENTER_CRITICAL(&s_status_lock);
                ++s_status.hardware_errors;
                taskEXIT_CRITICAL(&s_status_lock);
                ESP_LOGE(TAG, "scene blend apply failed: %s",
                         esp_err_to_name(hw_err));
            }
        }

        /*
         * Mantem o periodo total proximo de 20 ms, descontando o tempo de
         * render. Se o frame ja excedeu o periodo, bloqueia por um tick para
         * IDLE1 alimentar o watchdog, sem somar 20 ms inteiros ao frame.
         */
        const TickType_t period = pdMS_TO_TICKS(NB_HEAD_DISPLAY_TICK_MS);
        const TickType_t elapsed = xTaskGetTickCount() - cycle_started;
        vTaskDelay(elapsed < period ? period - elapsed : 1U);
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
        &s_display_task_handle,
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

uint32_t nb_head_display_service_get_stack_min_free_words(void)
{
    if (s_display_task_handle == NULL) {
        return 0U;
    }
    return (uint32_t)uxTaskGetStackHighWaterMark(s_display_task_handle);
}

esp_err_t nb_head_display_service_set_status_icons(uint32_t icon_bits)
{
    if (!s_status.enabled) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    taskENTER_CRITICAL(&s_status_lock);
    if (s_status_icon_bits != icon_bits) {
        s_status_icon_bits = icon_bits;
        s_status_icon_bits_dirty = true;
    }
    taskEXIT_CRITICAL(&s_status_lock);
    return ESP_OK;
}
