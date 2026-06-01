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

#define NB_AUDIO_PLAYBACK_V2_QUEUE_PACKETS  16U
#define NB_AUDIO_PLAYBACK_V2_SAMPLE_RATE_HZ 16000U
#define NB_AUDIO_PLAYBACK_V2_CHUNK_SAMPLES  256U
#define NB_AUDIO_PLAYBACK_V2_PROBE_HZ       440U

typedef struct {
    bool initialized;
    bool playing;
    bool stop_requested;
    bool bridge_say_observer;
    bool bridge_say_queue_owner;
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

typedef struct {
    int16_t samples[NB_AUDIO_PLAYBACK_V2_CHUNK_SAMPLES];
    uint16_t count;
} nb_audio_playback_v2_say_chunk_t;

esp_err_t audio_playback_service_v2_init(void);
esp_err_t audio_playback_service_v2_deinit(void);
bool audio_playback_service_v2_is_initialized(void);
void audio_playback_service_v2_get_status(nb_audio_playback_v2_status_t *out);

esp_err_t audio_playback_service_v2_probe_start(uint32_t duration_ms, uint16_t amplitude);
esp_err_t audio_playback_service_v2_probe_stop(void);
bool audio_playback_service_v2_is_playing(void);
bool audio_playback_service_v2_fill_probe_chunk(int16_t *out, uint16_t sample_count);

esp_err_t audio_playback_service_v2_say_enqueue(const int16_t *samples, uint16_t count);
bool audio_playback_service_v2_say_dequeue(nb_audio_playback_v2_say_chunk_t *out);
uint32_t audio_playback_service_v2_say_cancel(void);
uint32_t audio_playback_service_v2_say_pending_count(void);

void audio_playback_service_v2_note_say_queue_depth(uint32_t depth);
void audio_playback_service_v2_note_say_enqueued(uint32_t queue_count);
void audio_playback_service_v2_note_say_played(uint32_t queue_count);
void audio_playback_service_v2_note_say_dropped(uint32_t queue_count, bool during_listen);
void audio_playback_service_v2_note_say_cancelled(uint32_t pending_chunks);
void audio_playback_service_v2_note_say_idle(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_AUDIO_PLAYBACK_SERVICE_V2_H */
