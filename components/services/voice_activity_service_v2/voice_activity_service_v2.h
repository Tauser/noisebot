/*
 * voice_activity_service_v2.h - Voice activity v2 contract (Layer 4)
 *
 * Phase B skeleton only. VAD thresholds and wake policy remain in the current
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
    nb_voice_activity_v2_state_t state;
    uint32_t speech_frames;
    uint32_t silence_frames;
    uint32_t rms_last;
    uint32_t peak_last;
} nb_voice_activity_v2_status_t;

esp_err_t voice_activity_service_v2_init(void);
esp_err_t voice_activity_service_v2_deinit(void);
bool voice_activity_service_v2_is_initialized(void);
void voice_activity_service_v2_get_status(nb_voice_activity_v2_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NB_VOICE_ACTIVITY_SERVICE_V2_H */
