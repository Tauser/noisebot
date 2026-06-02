/*
 * voice_activity_service_v2.c - passive Voice Activity v2 shadow probe.
 */

#include "voice_activity_service_v2.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <string.h>

#define ACTIVITY_V2_CHUNK_MS            16U
#define SHADOW_MIN_DURATION_MS          16U
#define SHADOW_MAX_DURATION_MS          30000U
#define SHADOW_SPEECH_RMS_THRESHOLD     1200U
#define SHADOW_SPEECH_PEAK_THRESHOLD    2400U
#define SESSION_SPEECH_RMS_THRESHOLD    200U
#define SESSION_SPEECH_PEAK_THRESHOLD   700U
#define SESSION_END_SILENCE_MS          900U

static nb_voice_activity_v2_status_t s_status = {
    .state = NB_VOICE_ACTIVITY_V2_STATE_UNKNOWN,
};
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t isqrt_u64(uint64_t value)
{
    uint64_t bit = 1ULL << 62;
    uint64_t result = 0;

    while (bit > value) {
        bit >>= 2;
    }

    while (bit != 0ULL) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }

    return (uint32_t)result;
}

static uint32_t zcr_permille(const int16_t *samples, uint16_t sample_count)
{
    if (samples == NULL || sample_count < 2U) {
        return 0U;
    }

    int8_t prev_sign = 0;
    uint32_t crossings = 0;
    for (uint16_t i = 0; i < sample_count; i++) {
        int16_t sample = samples[i];
        int8_t sign = (sample > 0) ? 1 : ((sample < 0) ? -1 : 0);
        if (sign == 0) {
            continue;
        }
        if (prev_sign != 0 && sign != prev_sign) {
            crossings++;
        }
        prev_sign = sign;
    }

    return (crossings * 1000U) / (uint32_t)(sample_count - 1U);
}

esp_err_t voice_activity_service_v2_init(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (s_status.initialized) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = true;
    s_status.state = NB_VOICE_ACTIVITY_V2_STATE_UNKNOWN;
    s_status.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t voice_activity_service_v2_deinit(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (!s_status.initialized) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = NB_VOICE_ACTIVITY_V2_STATE_UNKNOWN;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

bool voice_activity_service_v2_is_initialized(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool initialized = s_status.initialized;
    taskEXIT_CRITICAL(&s_mux);
    return initialized;
}

void voice_activity_service_v2_get_status(nb_voice_activity_v2_status_t *out)
{
    if (out == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_mux);
    *out = s_status;
    taskEXIT_CRITICAL(&s_mux);
}

esp_err_t voice_activity_service_v2_shadow_start(uint32_t duration_ms)
{
    if (duration_ms < SHADOW_MIN_DURATION_MS || duration_ms > SHADOW_MAX_DURATION_MS) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_mux);
    if (!s_status.initialized) {
        memset(&s_status, 0, sizeof(s_status));
        s_status.initialized = true;
    }
    if (s_status.shadow_running) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    bool initialized = s_status.initialized;
    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = initialized;
    s_status.shadow_running = true;
    s_status.shadow_duration_ms = duration_ms;
    s_status.state = NB_VOICE_ACTIVITY_V2_STATE_UNKNOWN;
    s_status.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t voice_activity_service_v2_shadow_stop(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (!s_status.shadow_running) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    s_status.shadow_running = false;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

bool voice_activity_service_v2_shadow_is_running(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool running = s_status.shadow_running;
    taskEXIT_CRITICAL(&s_mux);
    return running;
}

esp_err_t voice_activity_service_v2_session_compare_begin(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (!s_status.initialized) {
        memset(&s_status, 0, sizeof(s_status));
        s_status.initialized = true;
    }

    bool initialized = s_status.initialized;
    bool shadow_running = s_status.shadow_running;
    uint32_t shadow_duration_ms = s_status.shadow_duration_ms;
    uint32_t session_compare_id = s_status.session_compare_id + 1U;
    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = initialized;
    s_status.shadow_running = shadow_running;
    s_status.shadow_duration_ms = shadow_duration_ms;
    s_status.session_compare_active = true;
    s_status.session_compare_id = session_compare_id;
    s_status.state = NB_VOICE_ACTIVITY_V2_STATE_UNKNOWN;
    s_status.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

void voice_activity_service_v2_session_compare_legacy_end(uint32_t reason,
                                                          uint32_t elapsed_ms)
{
    taskENTER_CRITICAL(&s_mux);
    if (!s_status.initialized || !s_status.session_compare_active) {
        taskEXIT_CRITICAL(&s_mux);
        return;
    }

    s_status.session_compare_active = false;
    s_status.session_active = false;
    s_status.legacy_end_observed = true;
    s_status.legacy_end_reason = reason;
    s_status.legacy_end_elapsed_ms = elapsed_ms;
    s_status.decision_diverged =
        (s_status.activity_end_observed && reason != 0U) ||
        (!s_status.activity_end_observed && reason == 0U);
    taskEXIT_CRITICAL(&s_mux);
}

void voice_activity_service_v2_feed_frame(const int16_t *samples,
                                          uint16_t sample_count,
                                          bool session_active,
                                          bool muted)
{
    if (samples == NULL || sample_count == 0U) {
        return;
    }

    taskENTER_CRITICAL(&s_mux);
    bool shadow_running = s_status.shadow_running;
    bool session_compare_running = s_status.session_compare_active;
    bool running = shadow_running || session_compare_running;
    taskEXIT_CRITICAL(&s_mux);
    if (!running) {
        return;
    }

    uint64_t sum_sq = 0;
    uint32_t peak = 0;
    for (uint16_t i = 0; i < sample_count; i++) {
        int32_t v = samples[i];
        uint32_t mag = (uint32_t)((v < 0) ? -v : v);
        sum_sq += (uint64_t)mag * (uint64_t)mag;
        if (mag > peak) {
            peak = mag;
        }
    }

    uint32_t rms = isqrt_u64(sum_sq / sample_count);
    uint32_t zcr = zcr_permille(samples, sample_count);
    uint32_t speech_rms_threshold = (session_compare_running && session_active)
        ? SESSION_SPEECH_RMS_THRESHOLD
        : SHADOW_SPEECH_RMS_THRESHOLD;
    uint32_t speech_peak_threshold = (session_compare_running && session_active)
        ? SESSION_SPEECH_PEAK_THRESHOLD
        : SHADOW_SPEECH_PEAK_THRESHOLD;
    bool speech = !muted &&
                  (rms >= speech_rms_threshold ||
                   peak >= speech_peak_threshold);

    taskENTER_CRITICAL(&s_mux);
    if (!s_status.shadow_running && !s_status.session_compare_active) {
        taskEXIT_CRITICAL(&s_mux);
        return;
    }

    s_status.session_active = session_active;
    s_status.observed_frames++;
    if (session_active) {
        s_status.session_frames++;
    } else {
        s_status.idle_frames++;
    }
    s_status.rms_last = rms;
    s_status.peak_last = peak;
    s_status.zcr_last_permille = zcr;
    if (rms > s_status.rms_max) {
        s_status.rms_max = rms;
    }
    if (peak > s_status.peak_max) {
        s_status.peak_max = peak;
    }
    if (zcr > s_status.zcr_max_permille) {
        s_status.zcr_max_permille = zcr;
    }
    if (muted) {
        s_status.muted_frames++;
        if (rms > s_status.muted_rms_max) {
            s_status.muted_rms_max = rms;
        }
        if (peak > s_status.muted_peak_max) {
            s_status.muted_peak_max = peak;
        }
        if (zcr > s_status.muted_zcr_max_permille) {
            s_status.muted_zcr_max_permille = zcr;
        }
    } else {
        s_status.unmuted_frames++;
        if (rms > s_status.unmuted_rms_max) {
            s_status.unmuted_rms_max = rms;
        }
        if (peak > s_status.unmuted_peak_max) {
            s_status.unmuted_peak_max = peak;
        }
        if (zcr > s_status.unmuted_zcr_max_permille) {
            s_status.unmuted_zcr_max_permille = zcr;
        }
    }
    if (speech) {
        if (s_status.session_compare_active && session_active) {
            s_status.session_compare_speech_seen = true;
        }
        s_status.speech_frames++;
        s_status.speech_run_frames++;
        s_status.silence_run_frames = 0U;
        if (s_status.speech_run_frames > s_status.speech_run_max_frames) {
            s_status.speech_run_max_frames = s_status.speech_run_frames;
        }
        s_status.state = NB_VOICE_ACTIVITY_V2_STATE_SPEECH;
    } else {
        s_status.silence_frames++;
        s_status.silence_run_frames++;
        s_status.speech_run_frames = 0U;
        if (s_status.silence_run_frames > s_status.silence_run_max_frames) {
            s_status.silence_run_max_frames = s_status.silence_run_frames;
        }
        s_status.state = NB_VOICE_ACTIVITY_V2_STATE_SILENCE;
        if (s_status.session_compare_active &&
            session_active &&
            s_status.session_compare_speech_seen &&
            !s_status.activity_end_observed &&
            (s_status.silence_run_frames * ACTIVITY_V2_CHUNK_MS) >= SESSION_END_SILENCE_MS) {
            s_status.activity_end_observed = true;
            s_status.activity_end_elapsed_ms = s_status.shadow_elapsed_ms + ACTIVITY_V2_CHUNK_MS;
        }
    }

    s_status.shadow_elapsed_ms += ACTIVITY_V2_CHUNK_MS;
    if (s_status.shadow_running &&
        s_status.shadow_elapsed_ms >= s_status.shadow_duration_ms) {
        s_status.shadow_running = false;
    }
    taskEXIT_CRITICAL(&s_mux);
}
