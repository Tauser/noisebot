/*
 * audio_playback_service_v2.c - inactive Playback v2 skeleton.
 */

#include "audio_playback_service_v2.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <string.h>

#define PLAYBACK_PROBE_MIN_DURATION_MS  16U
#define PLAYBACK_PROBE_MAX_DURATION_MS  2000U
#define PLAYBACK_PROBE_DEFAULT_AMP      1200U
#define PLAYBACK_PROBE_MAX_AMP          6000U

static nb_audio_playback_v2_status_t s_status;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_phase;

esp_err_t audio_playback_service_v2_init(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (s_status.initialized) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = true;
    s_status.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t audio_playback_service_v2_deinit(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (!s_status.initialized) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_phase = 0;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

bool audio_playback_service_v2_is_initialized(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool initialized = s_status.initialized;
    taskEXIT_CRITICAL(&s_mux);
    return initialized;
}

void audio_playback_service_v2_get_status(nb_audio_playback_v2_status_t *out)
{
    if (out == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_mux);
    *out = s_status;
    taskEXIT_CRITICAL(&s_mux);
}

esp_err_t audio_playback_service_v2_probe_start(uint32_t duration_ms, uint16_t amplitude)
{
    if (duration_ms < PLAYBACK_PROBE_MIN_DURATION_MS ||
        duration_ms > PLAYBACK_PROBE_MAX_DURATION_MS ||
        amplitude > PLAYBACK_PROBE_MAX_AMP) {
        return ESP_ERR_INVALID_ARG;
    }

    if (amplitude == 0U) {
        amplitude = PLAYBACK_PROBE_DEFAULT_AMP;
    }

    uint32_t chunks = (duration_ms + 15U) / 16U;
    taskENTER_CRITICAL(&s_mux);
    if (!s_status.initialized) {
        memset(&s_status, 0, sizeof(s_status));
        s_status.initialized = true;
    }
    if (s_status.playing) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    s_status.playing = true;
    s_status.stop_requested = false;
    s_status.probe_duration_ms = duration_ms;
    s_status.probe_elapsed_ms = 0;
    s_status.queued_chunks = chunks;
    s_status.played_chunks = 0;
    s_status.dropped_chunks = 0;
    s_status.amplitude = amplitude;
    s_status.last_error = ESP_OK;
    s_phase = 0;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t audio_playback_service_v2_probe_stop(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (!s_status.playing) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    s_status.playing = false;
    s_status.stop_requested = true;
    s_status.queued_chunks = 0;
    s_status.cancel_count++;
    s_phase = 0;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

bool audio_playback_service_v2_is_playing(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool playing = s_status.playing;
    taskEXIT_CRITICAL(&s_mux);
    return playing;
}

bool audio_playback_service_v2_fill_probe_chunk(int16_t *out, uint16_t sample_count)
{
    if (out == NULL || sample_count == 0U) {
        return false;
    }

    taskENTER_CRITICAL(&s_mux);
    bool playing = s_status.playing;
    uint32_t amplitude = s_status.amplitude;
    uint32_t phase = s_phase;
    taskEXIT_CRITICAL(&s_mux);
    if (!playing) {
        return false;
    }

    uint32_t half_period = NB_AUDIO_PLAYBACK_V2_SAMPLE_RATE_HZ /
                           (NB_AUDIO_PLAYBACK_V2_PROBE_HZ * 2U);
    if (half_period == 0U) {
        half_period = 1U;
    }

    for (uint16_t i = 0; i < sample_count; i++) {
        out[i] = ((phase / half_period) & 1U) == 0U
               ? (int16_t)amplitude
               : (int16_t)(-(int32_t)amplitude);
        phase++;
    }

    taskENTER_CRITICAL(&s_mux);
    if (!s_status.playing) {
        taskEXIT_CRITICAL(&s_mux);
        memset(out, 0, sample_count * sizeof(int16_t));
        return false;
    }

    s_phase = phase;
    s_status.played_chunks++;
    if (s_status.queued_chunks > 0U) {
        s_status.queued_chunks--;
    }
    s_status.probe_elapsed_ms += 16U;
    if (s_status.queued_chunks == 0U ||
        s_status.probe_elapsed_ms >= s_status.probe_duration_ms) {
        s_status.playing = false;
    }
    taskEXIT_CRITICAL(&s_mux);
    return true;
}
