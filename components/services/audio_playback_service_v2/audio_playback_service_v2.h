/*
 * audio_playback_service_v2.h - Playback v2 contract (Layer 4)
 *
 * Phase B skeleton only. It does not consume SAY chunks or drive I2S yet.
 */

#ifndef NB_AUDIO_PLAYBACK_SERVICE_V2_H
#define NB_AUDIO_PLAYBACK_SERVICE_V2_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_AUDIO_PLAYBACK_V2_QUEUE_PACKETS  8U

typedef struct {
    bool initialized;
    bool playing;
    uint32_t queued_chunks;
    uint32_t dropped_chunks;
    uint32_t cancel_count;
} nb_audio_playback_v2_status_t;

esp_err_t audio_playback_service_v2_init(void);
esp_err_t audio_playback_service_v2_deinit(void);
bool audio_playback_service_v2_is_initialized(void);
void audio_playback_service_v2_get_status(nb_audio_playback_v2_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NB_AUDIO_PLAYBACK_SERVICE_V2_H */
