/*
 * audio_codec_service_v2.c - inactive Codec v2 skeleton.
 */

#include "audio_codec_service_v2.h"
#include <string.h>

static nb_audio_codec_v2_status_t s_status = {
    .format = NB_AUDIO_CODEC_V2_FORMAT_PCM16,
};

esp_err_t audio_codec_service_v2_init(void)
{
    if (s_status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = true;
    s_status.format = NB_AUDIO_CODEC_V2_FORMAT_PCM16;
    return ESP_OK;
}

esp_err_t audio_codec_service_v2_deinit(void)
{
    if (!s_status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.format = NB_AUDIO_CODEC_V2_FORMAT_PCM16;
    return ESP_OK;
}

bool audio_codec_service_v2_is_initialized(void)
{
    return s_status.initialized;
}

void audio_codec_service_v2_get_status(nb_audio_codec_v2_status_t *out)
{
    if (out == NULL) {
        return;
    }

    *out = s_status;
}
