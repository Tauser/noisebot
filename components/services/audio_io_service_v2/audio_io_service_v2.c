/*
 * audio_io_service_v2.c - inactive Audio I/O v2 skeleton.
 */

#include "audio_io_service_v2.h"
#include <string.h>

static nb_audio_io_v2_status_t s_status;

esp_err_t audio_io_service_v2_init(void)
{
    if (s_status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = true;
    return ESP_OK;
}

esp_err_t audio_io_service_v2_deinit(void)
{
    if (!s_status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_status, 0, sizeof(s_status));
    return ESP_OK;
}

bool audio_io_service_v2_is_initialized(void)
{
    return s_status.initialized;
}

void audio_io_service_v2_get_status(nb_audio_io_v2_status_t *out)
{
    if (out == NULL) {
        return;
    }

    *out = s_status;
}
