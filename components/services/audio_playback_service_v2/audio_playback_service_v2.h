/*
 * audio_playback_service_v2.h - Playback v2 contract (Layer 4)
 *
 * Playback v2 probe does not own I2S. The active audio_service pulls synthetic
 * frames explicitly while the probe is running.
 */

#ifndef NB_AUDIO_PLAYBACK_SERVICE_V2_H
#define NB_AUDIO_PLAYBACK_SERVICE_V2_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool initialized;
    bool playing;
    bool stop_requested;
    bool bridge_say_observer;
    bool bridge_say_queue_owner;
    bool speaker_owner_dry_run_enabled;
    bool speaker_owner_requested;
    bool speaker_owner_ready;
    bool speaker_owner_active;
    bool speaker_owner_candidate;
    bool speaker_owner_handoff_ready;
    uint32_t speaker_owner_block_reason;
    bool speaker_owner_real_requested;
    bool speaker_owner_real_armed;
    uint32_t speaker_owner_real_block_reason;
    bool speaker_owner_real_window_active;
    bool speaker_owner_real_window_completed;
    uint32_t speaker_owner_real_auto_disarm_count;
    uint32_t speaker_owner_real_write_frames;
    uint32_t speaker_owner_real_write_samples;
    uint32_t speaker_owner_real_write_failures;
    esp_err_t speaker_owner_real_last_result;
    uint32_t speaker_owner_frames;
    uint32_t speaker_owner_samples;
    uint32_t speaker_owner_silence_frames;
    uint32_t speaker_owner_failures;
    uint32_t speaker_owner_recoveries;
    uint32_t speaker_owner_last_samples;
    esp_err_t speaker_owner_last_result;
    uint32_t speaker_frames_prepared;
    uint32_t speaker_samples_prepared;
    uint32_t speaker_last_samples;
    uint32_t speaker_last_volume;
    uint32_t speaker_frames_committed;
    uint32_t speaker_samples_committed;
    uint32_t speaker_commit_failures;
    uint32_t speaker_last_commit_samples;
    esp_err_t speaker_last_commit_result;
    uint32_t speaker_write_requests;
    uint32_t speaker_write_samples;
    uint32_t speaker_write_failures;
    uint32_t speaker_last_write_samples;
    esp_err_t speaker_last_write_result;
    uint32_t speaker_empty_polls;
    uint32_t speaker_empty_ms;
    uint32_t speaker_idle_end_count;
    uint32_t probe_duration_ms;
    uint32_t probe_elapsed_ms;
    uint32_t queued_chunks;
    uint32_t played_chunks;
    uint32_t dropped_chunks;
    uint32_t cancel_count;
    uint32_t amplitude;
    uint32_t say_queue_depth;
    uint32_t say_queue_count;
    uint32_t say_chunks_received;
    uint32_t say_chunks_played;
    uint32_t say_chunks_dropped;
    uint32_t say_chunks_dropped_listening;
    uint32_t say_chunks_cancelled;
    uint32_t say_cancel_count;
    esp_err_t last_error;
} nb_audio_playback_v2_status_t;

typedef esp_err_t (*nb_audio_playback_v2_speaker_write_cb_t)(
    const int16_t *samples,
    uint16_t sample_count,
    void *ctx);

esp_err_t audio_playback_service_v2_init(void);
esp_err_t audio_playback_service_v2_deinit(void);
bool audio_playback_service_v2_is_initialized(void);
void audio_playback_service_v2_get_status(nb_audio_playback_v2_status_t *out);

esp_err_t audio_playback_service_v2_probe_start(uint32_t duration_ms, uint16_t amplitude);
esp_err_t audio_playback_service_v2_probe_stop(void);
bool audio_playback_service_v2_is_playing(void);
bool audio_playback_service_v2_fill_probe_chunk(int16_t *out, uint16_t sample_count);
esp_err_t audio_playback_service_v2_speaker_owner_arm(void);
esp_err_t audio_playback_service_v2_speaker_owner_disarm(void);
esp_err_t audio_playback_service_v2_speaker_owner_real_arm(void);
esp_err_t audio_playback_service_v2_speaker_owner_real_disarm(void);
esp_err_t audio_playback_service_v2_say_accept(const int16_t *samples, uint16_t count);
bool audio_playback_service_v2_speaker_write_next_frame(
    uint8_t volume_percent,
    nb_audio_playback_v2_speaker_write_cb_t write_cb,
    void *ctx,
    uint16_t *sample_count,
    esp_err_t *result);
bool audio_playback_service_v2_speaker_should_end_idle(void);
uint32_t audio_playback_service_v2_say_cancel_active(void);
void audio_playback_service_v2_say_drop_listening(void);
void audio_playback_service_v2_say_end_idle(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_AUDIO_PLAYBACK_SERVICE_V2_H */
