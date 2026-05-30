/*
 * audio_io_service_v2.h - Audio I/O v2 contract (Layer 4)
 *
 * Phase B skeleton only. This service is intentionally not initialized from
 * boot; the current audio_service remains the active voice path.
 */

#ifndef NB_AUDIO_IO_SERVICE_V2_H
#define NB_AUDIO_IO_SERVICE_V2_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_AUDIO_IO_V2_SAMPLE_RATE_HZ      16000U
#define NB_AUDIO_IO_V2_CHANNELS            1U
#define NB_AUDIO_IO_V2_CHUNK_SAMPLES       256U
#define NB_AUDIO_IO_V2_CHUNK_MS            16U

typedef struct {
    const int16_t *samples;
    uint16_t sample_count;
    uint32_t timestamp_ms;
    uint8_t source_flags;
} nb_audio_io_v2_pcm_frame_t;

typedef struct {
    bool initialized;
    bool probe_running;
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t i2s_recoveries;
    uint32_t dropped_frames;
} nb_audio_io_v2_status_t;

esp_err_t audio_io_service_v2_init(void);
esp_err_t audio_io_service_v2_deinit(void);
bool audio_io_service_v2_is_initialized(void);
void audio_io_service_v2_get_status(nb_audio_io_v2_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NB_AUDIO_IO_SERVICE_V2_H */
