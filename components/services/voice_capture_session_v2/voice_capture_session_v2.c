/*
 * voice_capture_session_v2.c - inactive Capture Session v2 state machine.
 */

#include "voice_capture_session_v2.h"
#include "bridge_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_timer.h"
#include <string.h>

#define CAPTURE_REPLAY_FRAME_MS        16U
#define CAPTURE_REPLAY_SAMPLE_RATE_HZ  16000U
#define CAPTURE_REPLAY_MIN_TOTAL_MS    CAPTURE_REPLAY_FRAME_MS
#define CAPTURE_REPLAY_MAX_TOTAL_MS    12000U
#define CAPTURE_REPLAY_END_FRAMES      \
    ((NB_VOICE_CAPTURE_V2_END_SILENCE_MS + CAPTURE_REPLAY_FRAME_MS - 1U) / \
     CAPTURE_REPLAY_FRAME_MS)

static nb_voice_capture_v2_status_t s_status = {
    .state = NB_VOICE_CAPTURE_V2_IDLE_SESSION,
    .source = NB_VOICE_CAPTURE_V2_SOURCE_WAKE_WORD,
};
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_next_session_id = 1U;

static void update_handoff_gate_locked(void)
{
    s_status.bridge_tx_candidate = s_status.real_capture &&
                                   s_status.legacy_audio_service_tx_owner &&
                                   !s_status.bridge_tx_owner;
    s_status.bridge_tx_handoff_ready = false;
    s_status.handoff_block_reason = NB_VOICE_CAPTURE_V2_HANDOFF_BLOCK_NONE;

    if (s_status.bridge_tx_owner) {
        s_status.handoff_block_reason = NB_VOICE_CAPTURE_V2_HANDOFF_BLOCK_ALREADY_OWNER;
    } else if (!s_status.real_capture) {
        s_status.handoff_block_reason = NB_VOICE_CAPTURE_V2_HANDOFF_BLOCK_NOT_REAL_CAPTURE;
    } else if (s_status.session_active) {
        s_status.handoff_block_reason = NB_VOICE_CAPTURE_V2_HANDOFF_BLOCK_SESSION_ACTIVE;
    } else if (s_status.end_reason != NB_VOICE_CAPTURE_V2_END_SPEECH_COMPLETE) {
        s_status.handoff_block_reason = NB_VOICE_CAPTURE_V2_HANDOFF_BLOCK_END_REASON;
    } else if (!s_status.shadow_voice_start_sent ||
               !s_status.shadow_voice_end_sent ||
               s_status.shadow_audio_chunks == 0U ||
               s_status.shadow_audio_samples == 0U) {
        s_status.handoff_block_reason = NB_VOICE_CAPTURE_V2_HANDOFF_BLOCK_NO_AUDIO;
    } else if (s_status.shadow_audio_dropped_chunks > 0U ||
               s_status.dropped_frames > 0U) {
        s_status.handoff_block_reason = NB_VOICE_CAPTURE_V2_HANDOFF_BLOCK_DROPPED_AUDIO;
    } else {
        s_status.bridge_tx_handoff_ready = true;
    }
}

static void reset_runtime_locked(bool keep_initialized)
{
    bool initialized = keep_initialized && s_status.initialized;
    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = initialized;
    s_status.state = NB_VOICE_CAPTURE_V2_IDLE_SESSION;
    s_status.source = NB_VOICE_CAPTURE_V2_SOURCE_WAKE_WORD;
    s_status.end_reason = NB_VOICE_CAPTURE_V2_END_NONE;
    s_status.bridge_tx_owner = false;
    s_status.legacy_audio_service_tx_owner = true;
    s_status.last_error = ESP_OK;
    update_handoff_gate_locked();
}

esp_err_t voice_capture_session_v2_init(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (s_status.initialized) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    reset_runtime_locked(false);
    s_status.initialized = true;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t voice_capture_session_v2_deinit(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (!s_status.initialized) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    reset_runtime_locked(false);
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

bool voice_capture_session_v2_is_initialized(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool initialized = s_status.initialized;
    taskEXIT_CRITICAL(&s_mux);
    return initialized;
}

void voice_capture_session_v2_get_status(nb_voice_capture_v2_status_t *out)
{
    if (out == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_mux);
    update_handoff_gate_locked();
    *out = s_status;
    taskEXIT_CRITICAL(&s_mux);
}

esp_err_t voice_capture_session_v2_replay_start(nb_voice_capture_v2_source_t source,
                                                uint32_t speech_ms,
                                                uint32_t trailing_silence_ms)
{
    if ((uint32_t)source > (uint32_t)NB_VOICE_CAPTURE_V2_SOURCE_DEBUG) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t total_ms = speech_ms + trailing_silence_ms;
    if (total_ms < CAPTURE_REPLAY_MIN_TOTAL_MS ||
        total_ms > CAPTURE_REPLAY_MAX_TOTAL_MS ||
        speech_ms > NB_VOICE_CAPTURE_V2_MAX_SPEECH_MS) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t speech_frames = (speech_ms + CAPTURE_REPLAY_FRAME_MS - 1U) /
                             CAPTURE_REPLAY_FRAME_MS;
    uint32_t silence_frames = (trailing_silence_ms + CAPTURE_REPLAY_FRAME_MS - 1U) /
                              CAPTURE_REPLAY_FRAME_MS;

    taskENTER_CRITICAL(&s_mux);
    if (!s_status.initialized) {
        reset_runtime_locked(false);
        s_status.initialized = true;
    }
    if (s_status.session_active) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    reset_runtime_locked(true);
    s_status.session_active = true;
    s_status.real_capture = false;
    s_status.state = NB_VOICE_CAPTURE_V2_WAITING_FOR_SPEECH;
    s_status.source = source;
    s_status.session_id = s_next_session_id++;
    if (s_next_session_id == 0U) {
        s_next_session_id = 1U;
    }
    s_status.replay_duration_ms = total_ms;
    s_status.last_error = ESP_OK;

    if (speech_frames > 0U) {
        s_status.voice_start_sent = true;
        s_status.voice_audio_sent = true;
        s_status.shadow_voice_start_sent = true;
        s_status.state = NB_VOICE_CAPTURE_V2_CAPTURING;
        s_status.speech_frames = speech_frames;
        s_status.speech_elapsed_ms = speech_frames * CAPTURE_REPLAY_FRAME_MS;
        s_status.captured_samples = speech_frames *
                                    CAPTURE_REPLAY_FRAME_MS *
                                    (CAPTURE_REPLAY_SAMPLE_RATE_HZ / 1000U);
        s_status.shadow_audio_chunks = speech_frames;
        s_status.shadow_audio_samples = s_status.captured_samples;
    }

    if (silence_frames > 0U) {
        s_status.silence_frames = silence_frames;
        s_status.silence_elapsed_ms = silence_frames * CAPTURE_REPLAY_FRAME_MS;
        if (speech_frames > 0U && silence_frames < CAPTURE_REPLAY_END_FRAMES) {
            s_status.state = NB_VOICE_CAPTURE_V2_ENDING_ON_SILENCE;
        }
    }

    s_status.replay_elapsed_ms = s_status.speech_elapsed_ms +
                                 s_status.silence_elapsed_ms;
    if (speech_frames == 0U) {
        s_status.voice_start_sent = false;
        s_status.voice_audio_sent = false;
        s_status.end_reason = NB_VOICE_CAPTURE_V2_END_NO_SPEECH;
    } else {
        s_status.end_reason = NB_VOICE_CAPTURE_V2_END_SPEECH_COMPLETE;
    }
    s_status.voice_end_sent = speech_frames > 0U;
    s_status.shadow_voice_end_sent = s_status.voice_end_sent;
    s_status.session_active = false;
    s_status.state = NB_VOICE_CAPTURE_V2_DONE;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t voice_capture_session_v2_begin_real_pcm16(nb_voice_capture_v2_source_t source)
{
    if ((uint32_t)source > (uint32_t)NB_VOICE_CAPTURE_V2_SOURCE_DEBUG) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_mux);
    if (!s_status.initialized) {
        reset_runtime_locked(false);
        s_status.initialized = true;
    }
    if (s_status.session_active) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    reset_runtime_locked(true);
    s_status.source = source;
    s_status.session_active = true;
    s_status.real_capture = true;
    s_status.state = NB_VOICE_CAPTURE_V2_WAITING_FOR_SPEECH;
    s_status.end_reason = NB_VOICE_CAPTURE_V2_END_NONE;
    s_status.session_id = s_next_session_id++;
    if (s_next_session_id == 0U) {
        s_next_session_id = 1U;
    }
    s_status.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t voice_capture_session_v2_set_bridge_tx_owner(bool enabled)
{
    taskENTER_CRITICAL(&s_mux);
    if (!s_status.session_active || !s_status.real_capture) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    s_status.bridge_tx_owner = enabled;
    s_status.legacy_audio_service_tx_owner = !enabled;
    update_handoff_gate_locked();
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

void voice_capture_session_v2_note_voice_start(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (s_status.session_active && !s_status.voice_start_sent) {
        s_status.voice_start_sent = true;
        s_status.shadow_voice_start_sent = true;
        s_status.state = NB_VOICE_CAPTURE_V2_CAPTURING;
    }
    taskEXIT_CRITICAL(&s_mux);
}

void voice_capture_session_v2_note_audio_chunk(uint16_t sample_count, bool accepted)
{
    if (sample_count == 0U) {
        return;
    }

    uint32_t frame_units = 1U;
    uint32_t elapsed_ms = ((uint32_t)sample_count * 1000U) /
                          CAPTURE_REPLAY_SAMPLE_RATE_HZ;
    if (elapsed_ms == 0U) {
        elapsed_ms = CAPTURE_REPLAY_FRAME_MS;
    }
    if (sample_count >= 960U && (sample_count % 960U) == 0U) {
        frame_units = (uint32_t)sample_count / 960U;
    }

    taskENTER_CRITICAL(&s_mux);
    if (s_status.session_active) {
        if (accepted) {
            s_status.voice_audio_sent = true;
            s_status.captured_samples += sample_count;
            s_status.speech_frames += frame_units;
            s_status.speech_elapsed_ms += elapsed_ms;
            s_status.replay_elapsed_ms += elapsed_ms;
            s_status.shadow_audio_chunks += frame_units;
            s_status.shadow_audio_samples += sample_count;
        } else {
            s_status.dropped_frames++;
            s_status.shadow_audio_dropped_chunks++;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
}

esp_err_t voice_capture_session_v2_send_voice_start(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool can_send = s_status.session_active &&
                    s_status.real_capture &&
                    s_status.bridge_tx_owner &&
                    !s_status.voice_start_sent;
    taskEXIT_CRITICAL(&s_mux);

    if (!can_send) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!bridge_service_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    bridge_service_flush_tx();
    nb_event_t marker = {
        .type         = NB_EVT_VOICE_ACTIVITY_START,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
    };
    esp_err_t rc = bridge_service_send_event(&marker);
    if (rc != ESP_OK) {
        return rc;
    }

    voice_capture_session_v2_note_voice_start();
    return ESP_OK;
}

esp_err_t voice_capture_session_v2_send_audio_chunk(const int16_t *samples,
                                                    uint16_t sample_count)
{
    taskENTER_CRITICAL(&s_mux);
    bool can_send = s_status.session_active &&
                    s_status.real_capture &&
                    s_status.bridge_tx_owner &&
                    s_status.voice_start_sent;
    taskEXIT_CRITICAL(&s_mux);

    if (!can_send) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t rc = bridge_service_send_audio_chunk(samples, sample_count);
    voice_capture_session_v2_note_audio_chunk(sample_count, rc == ESP_OK);
    return rc;
}

esp_err_t voice_capture_session_v2_send_opus_packet(const uint8_t *packet,
                                                    uint16_t packet_len)
{
    taskENTER_CRITICAL(&s_mux);
    bool can_send = s_status.session_active &&
                    s_status.real_capture &&
                    s_status.bridge_tx_owner &&
                    s_status.voice_start_sent;
    taskEXIT_CRITICAL(&s_mux);

    if (!can_send) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t rc = bridge_service_send_opus_packet(packet, packet_len);
    voice_capture_session_v2_note_audio_chunk(NB_BRIDGE_OPUS_FRAME_SAMPLES,
                                              rc == ESP_OK);
    return rc;
}

esp_err_t voice_capture_session_v2_send_voice_end(uint32_t reason,
                                                  bool cancelled,
                                                  bool flush_before_end)
{
    taskENTER_CRITICAL(&s_mux);
    bool can_send = s_status.session_active &&
                    s_status.real_capture &&
                    s_status.bridge_tx_owner &&
                    s_status.voice_start_sent &&
                    (s_status.voice_audio_sent || cancelled);
    taskEXIT_CRITICAL(&s_mux);

    if (!can_send) {
        voice_capture_session_v2_finish(cancelled);
        return ESP_ERR_INVALID_STATE;
    }

    if (flush_before_end) {
        bridge_service_flush_tx();
    }

    nb_event_t marker = {
        .type         = NB_EVT_VOICE_ACTIVITY_END,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
        .data.u32     = reason,
    };
    esp_err_t rc = bridge_service_send_event(&marker);

    taskENTER_CRITICAL(&s_mux);
    if (s_status.session_active) {
        s_status.session_active = false;
        s_status.state = cancelled
            ? NB_VOICE_CAPTURE_V2_CANCELLED
            : NB_VOICE_CAPTURE_V2_DONE;
        s_status.voice_end_sent = (rc == ESP_OK);
        s_status.shadow_voice_end_sent = s_status.voice_end_sent;
        if (rc == ESP_OK) {
            s_status.end_reason = cancelled
                ? NB_VOICE_CAPTURE_V2_END_CANCELLED
                : NB_VOICE_CAPTURE_V2_END_SPEECH_COMPLETE;
        } else {
            s_status.end_reason = NB_VOICE_CAPTURE_V2_END_ERROR;
        }
        s_status.last_error = rc;
        update_handoff_gate_locked();
    }
    taskEXIT_CRITICAL(&s_mux);
    return rc;
}

void voice_capture_session_v2_finish(bool cancelled)
{
    taskENTER_CRITICAL(&s_mux);
    if (!s_status.session_active) {
        taskEXIT_CRITICAL(&s_mux);
        return;
    }

    s_status.session_active = false;
    if (cancelled) {
        s_status.state = NB_VOICE_CAPTURE_V2_CANCELLED;
        s_status.voice_end_sent = false;
        s_status.shadow_voice_end_sent = false;
        s_status.end_reason = NB_VOICE_CAPTURE_V2_END_CANCELLED;
    } else {
        s_status.state = NB_VOICE_CAPTURE_V2_DONE;
        s_status.voice_end_sent = s_status.voice_start_sent && s_status.voice_audio_sent;
        s_status.shadow_voice_end_sent = s_status.voice_end_sent;
        s_status.end_reason = s_status.voice_audio_sent
                                  ? NB_VOICE_CAPTURE_V2_END_SPEECH_COMPLETE
                                  : NB_VOICE_CAPTURE_V2_END_NO_SPEECH;
    }
    s_status.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_mux);
}

esp_err_t voice_capture_session_v2_cancel(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (!s_status.session_active &&
        s_status.state != NB_VOICE_CAPTURE_V2_WAITING_FOR_SPEECH &&
        s_status.state != NB_VOICE_CAPTURE_V2_CAPTURING &&
        s_status.state != NB_VOICE_CAPTURE_V2_ENDING_ON_SILENCE) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    s_status.session_active = false;
    s_status.state = NB_VOICE_CAPTURE_V2_CANCELLED;
    s_status.voice_end_sent = false;
    s_status.shadow_voice_end_sent = false;
    s_status.end_reason = NB_VOICE_CAPTURE_V2_END_CANCELLED;
    s_status.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

bool voice_capture_session_v2_is_active(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool active = s_status.session_active;
    taskEXIT_CRITICAL(&s_mux);
    return active;
}
