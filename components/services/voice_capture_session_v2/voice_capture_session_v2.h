/*
 * voice_capture_session_v2.h - Capture session v2 contract (Layer 4)
 *
 * Phase B skeleton only. The active wake/listen/session path remains v1.
 */

#ifndef NB_VOICE_CAPTURE_SESSION_V2_H
#define NB_VOICE_CAPTURE_SESSION_V2_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_VOICE_CAPTURE_V2_WAIT_FOR_SPEECH_MS  8000U
#define NB_VOICE_CAPTURE_V2_END_SILENCE_MS      900U
#define NB_VOICE_CAPTURE_V2_MAX_SPEECH_MS       9200U
#define NB_VOICE_CAPTURE_V2_PREROLL_CHUNKS      20U

typedef enum {
    NB_VOICE_CAPTURE_V2_IDLE_SESSION = 0,
    NB_VOICE_CAPTURE_V2_WAITING_FOR_SPEECH,
    NB_VOICE_CAPTURE_V2_CAPTURING,
    NB_VOICE_CAPTURE_V2_ENDING_ON_SILENCE,
    NB_VOICE_CAPTURE_V2_CANCELLED,
    NB_VOICE_CAPTURE_V2_DONE,
} nb_voice_capture_v2_state_t;

typedef enum {
    NB_VOICE_CAPTURE_V2_SOURCE_WAKE_WORD = 0,
    NB_VOICE_CAPTURE_V2_SOURCE_BARGE_IN,
    NB_VOICE_CAPTURE_V2_SOURCE_FOLLOWUP,
    NB_VOICE_CAPTURE_V2_SOURCE_DEBUG,
} nb_voice_capture_v2_source_t;

typedef enum {
    NB_VOICE_CAPTURE_V2_END_NONE = 0,
    NB_VOICE_CAPTURE_V2_END_SPEECH_COMPLETE,
    NB_VOICE_CAPTURE_V2_END_NO_SPEECH,
    NB_VOICE_CAPTURE_V2_END_CANCELLED,
    NB_VOICE_CAPTURE_V2_END_ERROR,
} nb_voice_capture_v2_end_reason_t;

typedef struct {
    bool initialized;
    bool session_active;
    bool real_capture;
    bool bridge_tx_owner;
    bool legacy_audio_service_tx_owner;
    bool voice_start_sent;
    bool voice_audio_sent;
    bool voice_end_sent;
    nb_voice_capture_v2_state_t state;
    nb_voice_capture_v2_source_t source;
    nb_voice_capture_v2_end_reason_t end_reason;
    uint32_t session_id;
    uint32_t replay_duration_ms;
    uint32_t replay_elapsed_ms;
    uint32_t speech_elapsed_ms;
    uint32_t silence_elapsed_ms;
    uint32_t speech_frames;
    uint32_t silence_frames;
    uint32_t captured_samples;
    uint32_t dropped_frames;
    esp_err_t last_error;
} nb_voice_capture_v2_status_t;

esp_err_t voice_capture_session_v2_init(void);
esp_err_t voice_capture_session_v2_deinit(void);
bool voice_capture_session_v2_is_initialized(void);
void voice_capture_session_v2_get_status(nb_voice_capture_v2_status_t *out);
esp_err_t voice_capture_session_v2_replay_start(nb_voice_capture_v2_source_t source,
                                                uint32_t speech_ms,
                                                uint32_t trailing_silence_ms);
esp_err_t voice_capture_session_v2_begin_real_pcm16(nb_voice_capture_v2_source_t source);
void voice_capture_session_v2_note_voice_start(void);
void voice_capture_session_v2_note_audio_chunk(uint16_t sample_count, bool accepted);
void voice_capture_session_v2_finish(bool cancelled);
esp_err_t voice_capture_session_v2_cancel(void);
bool voice_capture_session_v2_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_VOICE_CAPTURE_SESSION_V2_H */
