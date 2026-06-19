#include "nb_head_display_service.h"

#include <string.h>

#include "esp_log.h"

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

    ESP_LOGI(TAG, "DM2 display semantic receiver enabled; hardware deferred");
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

    memcpy(&s_status.scene, payload, sizeof(s_status.scene));
    s_status.scene_valid = true;
    ++s_status.accepted;
    return ESP_OK;
}

void nb_head_display_service_get_status(nb_head_display_status_t *out)
{
    if (out != NULL) {
        *out = s_status;
    }
}
