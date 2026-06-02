/*
 * audio_io_service_v2.h - Audio I/O v2 contract (Layer 4)
 *
 * Audio Service still owns the HAL path; v2 can accept normalized frames as a
 * logical owner before distribution while rollout validates the split.
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

typedef void (*nb_audio_io_v2_rx_consumer_cb_t)(
    const nb_audio_io_v2_pcm_frame_t *frame,
    void *ctx);

typedef struct {
    nb_audio_io_v2_rx_consumer_cb_t cb;
    void *ctx;
} nb_audio_io_v2_rx_consumer_t;

typedef enum {
    NB_AUDIO_IO_V2_SPEAKER_HANDOFF_BLOCK_NONE = 0,
    NB_AUDIO_IO_V2_SPEAKER_HANDOFF_BLOCK_DISABLED,
    NB_AUDIO_IO_V2_SPEAKER_HANDOFF_BLOCK_NO_TX,
    NB_AUDIO_IO_V2_SPEAKER_HANDOFF_BLOCK_TX_ERROR,
    NB_AUDIO_IO_V2_SPEAKER_HANDOFF_BLOCK_I2S_RECOVERY,
} nb_audio_io_v2_speaker_handoff_block_t;

typedef struct {
    bool initialized;
    bool probe_running;
    bool rx_owner_active;
    bool rx_owner_observed;
    bool session_rx_mirror_active;
    bool session_rx_mirror_observed;
    uint32_t probe_duration_ms;
    uint32_t probe_elapsed_ms;
    uint32_t rx_owner_frames;
    uint32_t rx_owner_samples;
    uint32_t rx_owner_last_samples;
    uint32_t rx_owner_source_flags;
    uint32_t rx_distributor_frames;
    uint32_t rx_distributor_samples;
    uint32_t rx_distributor_last_timestamp_ms;
    uint32_t rx_dispatch_calls;
    uint32_t rx_dispatch_consumers;
    uint32_t rx_dispatch_last_consumers;
    uint32_t session_rx_owner_frames;
    uint32_t session_rx_owner_samples;
    uint32_t session_rx_distributor_frames;
    uint32_t session_rx_distributor_samples;
    uint32_t session_rx_dispatch_calls;
    uint32_t session_rx_dispatch_consumers;
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
    bool tx_owner_observed;
    uint32_t tx_owner_frames;
    uint32_t tx_owner_samples;
    uint32_t tx_owner_last_samples;
    bool tx_owner_last_silence;
    esp_err_t tx_owner_last_result;
    bool speaker_handoff_supported;
    bool speaker_handoff_dry_run_enabled;
    bool speaker_handoff_owner_requested;
    bool speaker_handoff_owner_ready;
    bool speaker_handoff_active;
    bool speaker_handoff_candidate;
    bool speaker_handoff_ready;
    nb_audio_io_v2_speaker_handoff_block_t speaker_handoff_block_reason;
    uint32_t speaker_handoff_frames;
    uint32_t speaker_handoff_samples;
    uint32_t speaker_handoff_silence_frames;
    uint32_t speaker_handoff_failures;
    uint32_t speaker_handoff_recoveries;
    uint32_t speaker_handoff_last_samples;
    esp_err_t speaker_handoff_last_result;
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
esp_err_t audio_io_service_v2_set_speaker_handoff_dry_run(bool enabled);
esp_err_t audio_io_service_v2_set_speaker_handoff_owner_requested(bool requested);
void audio_io_service_v2_rx_owner_accept_frame(const int16_t *samples,
                                               uint16_t sample_count,
                                               uint32_t source_flags,
                                               nb_audio_io_v2_pcm_frame_t *out_frame);
void audio_io_service_v2_rx_dispatch_frame(const int16_t *samples,
                                           uint16_t sample_count,
                                           uint32_t source_flags,
                                           const nb_audio_io_v2_rx_consumer_t *consumers,
                                           uint8_t consumer_count,
                                           nb_audio_io_v2_pcm_frame_t *out_frame);
void audio_io_service_v2_probe_feed_rx_frame(const int16_t *samples, uint16_t sample_count);
void audio_io_service_v2_tx_owner_note_frame(uint16_t sample_count,
                                             bool silence,
                                             esp_err_t result);
void audio_io_service_v2_probe_note_tx_silence(esp_err_t result);
void audio_io_service_v2_note_i2s_recovery(esp_err_t reason);
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
