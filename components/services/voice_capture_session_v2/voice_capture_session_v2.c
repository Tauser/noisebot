/*
 * voice_capture_session_v2.c - inactive Capture Session v2 skeleton.
 */

#include "voice_capture_session_v2.h"
#include <string.h>

static nb_voice_capture_v2_status_t s_status = {
    .state = NB_VOICE_CAPTURE_V2_IDLE_SESSION,
    .source = NB_VOICE_CAPTURE_V2_SOURCE_WAKE_WORD,
};

esp_err_t voice_capture_session_v2_init(void)
{
    if (s_status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = true;
    s_status.state = NB_VOICE_CAPTURE_V2_IDLE_SESSION;
    s_status.source = NB_VOICE_CAPTURE_V2_SOURCE_WAKE_WORD;
    return ESP_OK;
}

esp_err_t voice_capture_session_v2_deinit(void)
{
    if (!s_status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = NB_VOICE_CAPTURE_V2_IDLE_SESSION;
    s_status.source = NB_VOICE_CAPTURE_V2_SOURCE_WAKE_WORD;
    return ESP_OK;
}

bool voice_capture_session_v2_is_initialized(void)
{
    return s_status.initialized;
}

void voice_capture_session_v2_get_status(nb_voice_capture_v2_status_t *out)
{
    if (out == NULL) {
        return;
    }

    *out = s_status;
}
