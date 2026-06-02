/*
 * voice_activity_service_v2.h - Voice activity v2 contract (Layer 4)
 *
 * Shadow/probe only. VAD thresholds and wake policy remain in the current
 * v1 services until a later flagged phase.
 */

#ifndef NB_VOICE_ACTIVITY_SERVICE_V2_H
#define NB_VOICE_ACTIVITY_SERVICE_V2_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NB_VOICE_ACTIVITY_V2_STATE_SILENCE = 0,
    NB_VOICE_ACTIVITY_V2_STATE_SPEECH,
    NB_VOICE_ACTIVITY_V2_STATE_UNKNOWN,
} nb_voice_activity_v2_state_t;

typedef struct {
    bool initialized;
    bool session_active;
    bool shadow_running;
    bool session_compare_active;
    bool session_compare_speech_seen;
    bool activity_end_observed;
    bool legacy_end_observed;
    bool decision_diverged;
    nb_voice_activity_v2_state_t state;
    uint32_t session_compare_id;
    uint32_t shadow_duration_ms;
    uint32_t shadow_elapsed_ms;
    uint32_t activity_end_elapsed_ms;
    uint32_t legacy_end_elapsed_ms;
    uint32_t legacy_end_reason;
    uint32_t observed_frames;
    uint32_t speech_frames;
    uint32_t silence_frames;
    uint32_t speech_run_frames;
    uint32_t silence_run_frames;
    uint32_t speech_run_max_frames;
    uint32_t silence_run_max_frames;
    uint32_t session_frames;
    uint32_t idle_frames;
    uint32_t rms_last;
    uint32_t peak_last;
    uint32_t zcr_last_permille;
    uint32_t rms_max;
    uint32_t peak_max;
    uint32_t zcr_max_permille;
    uint32_t muted_frames;
    uint32_t unmuted_frames;
    uint32_t muted_rms_max;
    uint32_t muted_peak_max;
    uint32_t muted_zcr_max_permille;
    uint32_t unmuted_rms_max;
    uint32_t unmuted_peak_max;
    uint32_t unmuted_zcr_max_permille;
    esp_err_t last_error;
} nb_voice_activity_v2_status_t;

esp_err_t voice_activity_service_v2_init(void);
esp_err_t voice_activity_service_v2_deinit(void);
bool voice_activity_service_v2_is_initialized(void);
void voice_activity_service_v2_get_status(nb_voice_activity_v2_status_t *out);
esp_err_t voice_activity_service_v2_shadow_start(uint32_t duration_ms);
esp_err_t voice_activity_service_v2_shadow_stop(void);
bool voice_activity_service_v2_shadow_is_running(void);
esp_err_t voice_activity_service_v2_session_compare_begin(void);
void voice_activity_service_v2_session_compare_legacy_end(uint32_t reason,
                                                          uint32_t elapsed_ms);
void voice_activity_service_v2_feed_frame(const int16_t *samples,
                                          uint16_t sample_count,
                                          bool session_active,
                                          bool muted);

#ifdef __cplusplus
}
#endif

#endif /* NB_VOICE_ACTIVITY_SERVICE_V2_H */
