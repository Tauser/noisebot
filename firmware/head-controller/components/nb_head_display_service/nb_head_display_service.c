#include "nb_head_display_service.h"

#include <string.h>

#include "esp_log.h"
#include "nb_head_display_hal.h"

#ifndef CONFIG_NB_HEAD_DISPLAY_ENABLED
#define CONFIG_NB_HEAD_DISPLAY_ENABLED 0
#endif

static const char *TAG = "nb_head_display";
static nb_head_display_status_t s_status;

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

    const nb_display_command_t *command =
        (const nb_display_command_t *)payload;
    if (s_status.scene_valid &&
        !nb_display_generation_is_newer(command->generation,
                                        s_status.scene.generation)) {
        ++s_status.ignored;
        ESP_LOGI(TAG,
                 "scene ignored generation=%lu current=%lu",
                 (unsigned long)command->generation,
                 (unsigned long)s_status.scene.generation);
        return ESP_OK;
    }

    if (s_status.hardware_ready) {
        const esp_err_t hw_err = nb_head_display_hal_apply(command);
        if (hw_err != ESP_OK) {
            ++s_status.hardware_errors;
            ESP_LOGE(TAG, "scene hardware apply failed: %s",
                     esp_err_to_name(hw_err));
            return hw_err;
        }
    }

    memcpy(&s_status.scene, command, sizeof(s_status.scene));
    s_status.scene_valid = true;
    ++s_status.accepted;
    ESP_LOGI(TAG,
             "scene accepted generation=%lu expression=%u gaze=%d,%d "
             "brightness=%u overlays=0x%04x",
             (unsigned long)s_status.scene.generation,
             (unsigned)s_status.scene.expression,
             (int)s_status.scene.gaze_x_milli,
             (int)s_status.scene.gaze_y_milli,
             (unsigned)s_status.scene.brightness,
             (unsigned)s_status.scene.overlay_flags);
    return ESP_OK;
}

void nb_head_display_service_get_status(nb_head_display_status_t *out)
{
    if (out != NULL) {
        s_status.spiram_free_bytes = nb_head_display_hal_spiram_free();
        *out = s_status;
    }
}
