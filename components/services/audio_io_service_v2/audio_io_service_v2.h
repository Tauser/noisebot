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
    bool session_rx_mirror_active;
    bool session_rx_mirror_observed;
    uint32_t probe_duration_ms;
    uint32_t probe_elapsed_ms;
    uint32_t session_rx_mirror_id;
    uint32_t session_rx_mirror_source;
    uint32_t session_rx_mirror_elapsed_ms;
    uint32_t session_rx_mirror_frames;
    uint32_t session_rx_mirror_samples;
    uint32_t session_rx_mirror_end_reason;
    bool session_rx_legacy_observed;
    bool session_rx_legacy_covered;
    uint32_t session_rx_legacy_frames;
    uint32_t session_rx_legacy_samples;
    uint32_t session_rx_legacy_elapsed_ms;
    uint32_t session_rx_compare_frame_delta;
    uint32_t session_rx_compare_sample_delta;
    uint32_t session_rx_compare_elapsed_delta_ms;
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t tx_silence_frames;
    uint32_t i2s_recoveries;
    uint32_t dropped_frames;
    uint32_t rms_last;
    uint32_t peak_last;
    uint32_t rms_max;
    uint32_t peak_max;
    uint32_t heap_internal_free_kb;
    uint32_t heap_dma_free_kb;
    esp_err_t last_error;
} nb_audio_io_v2_status_t;

esp_err_t audio_io_service_v2_init(void);
esp_err_t audio_io_service_v2_deinit(void);
bool audio_io_service_v2_is_initialized(void);
void audio_io_service_v2_get_status(nb_audio_io_v2_status_t *out);

esp_err_t audio_io_service_v2_probe_start(uint32_t duration_ms);
esp_err_t audio_io_service_v2_probe_stop(void);
bool audio_io_service_v2_probe_is_running(void);
void audio_io_service_v2_probe_feed_rx_frame(const int16_t *samples, uint16_t sample_count);
void audio_io_service_v2_probe_note_tx_silence(esp_err_t result);
esp_err_t audio_io_service_v2_session_rx_mirror_begin(uint32_t source);
void audio_io_service_v2_session_rx_mirror_feed(const int16_t *samples,
                                                uint16_t sample_count);
void audio_io_service_v2_session_rx_mirror_finish(uint32_t end_reason,
                                                  uint32_t legacy_frames,
                                                  uint32_t legacy_samples,
                                                  uint32_t legacy_elapsed_ms);

#ifdef __cplusplus
}
#endif

#endif /* NB_AUDIO_IO_SERVICE_V2_H */
