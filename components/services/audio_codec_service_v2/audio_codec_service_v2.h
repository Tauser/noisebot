/*
 * audio_codec_service_v2.h - Codec v2 contract (Layer 4)
 *
 * Phase B skeleton only. PCM16 remains the default transport and Opus stays
 * opt-in through the existing v1 experimental path.
 */

#ifndef NB_AUDIO_CODEC_SERVICE_V2_H
#define NB_AUDIO_CODEC_SERVICE_V2_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_AUDIO_CODEC_V2_SAMPLE_RATE_HZ     16000U
#define NB_AUDIO_CODEC_V2_CHANNELS           1U
#define NB_AUDIO_CODEC_V2_OPUS_FRAME_MS      60U
#define NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES 960U
#define NB_AUDIO_CODEC_V2_OPUS_BITRATE       32000U
#define NB_AUDIO_CODEC_V2_MAX_QUEUE_PACKETS  40U
#define NB_AUDIO_CODEC_V2_OVERFLOW_TEST_MAX_PACKETS 200U

typedef enum {
    NB_AUDIO_CODEC_V2_FORMAT_PCM16 = 0,
    NB_AUDIO_CODEC_V2_FORMAT_OPUS,
} nb_audio_codec_v2_format_t;

typedef enum {
    NB_AUDIO_CODEC_V2_WORKER_STATE_NOT_STARTED = 0,
} nb_audio_codec_v2_worker_state_t;

typedef struct {
    bool initialized;
    nb_audio_codec_v2_format_t format;
    nb_audio_codec_v2_worker_state_t worker_state;
    bool worker_supported;
    bool worker_active;
    uint32_t pcm_frames_in;
    uint32_t packets_out;
    uint32_t packet_drops;
    uint32_t queue_count;
    uint32_t opus_encode_tests;
    uint32_t opus_encoded_bytes_total;
    uint16_t opus_last_packet_bytes;
    int opus_codec_error;
    uint16_t pending_samples;
} nb_audio_codec_v2_status_t;

typedef struct {
    uint32_t attempted_packets;
    uint32_t accepted_packets;
    uint32_t dropped_packets;
    uint32_t peak_queue_count;
    uint32_t queue_count_after_cleanup;
    uint32_t status_packet_drops_after_cleanup;
} nb_audio_codec_v2_overflow_test_result_t;

typedef struct {
    uint16_t frame_samples;
    uint16_t outbuf_bytes;
    uint16_t encoded_bytes;
    uint32_t internal_before_kb;
    uint32_t internal_after_open_kb;
    uint32_t internal_after_close_kb;
    uint32_t dma_before_kb;
    uint32_t dma_after_open_kb;
    uint32_t dma_after_close_kb;
    uint32_t psram_before_kb;
    uint32_t psram_after_open_kb;
    uint32_t psram_after_close_kb;
    int codec_error;
} nb_audio_codec_v2_opus_test_result_t;

esp_err_t audio_codec_service_v2_init(void);
esp_err_t audio_codec_service_v2_deinit(void);
bool audio_codec_service_v2_is_initialized(void);
const char *audio_codec_service_v2_worker_state_name(nb_audio_codec_v2_worker_state_t state);
void audio_codec_service_v2_get_status(nb_audio_codec_v2_status_t *out);
esp_err_t audio_codec_service_v2_feed_pcm16(const int16_t *samples, uint16_t sample_count);
esp_err_t audio_codec_service_v2_encode_test_once(void);
esp_err_t audio_codec_service_v2_drain_synthetic(uint32_t *drained_packets);
esp_err_t audio_codec_service_v2_reset_diagnostics(void);
esp_err_t audio_codec_service_v2_overflow_test(
    uint32_t packets,
    nb_audio_codec_v2_overflow_test_result_t *out);
esp_err_t audio_codec_service_v2_opus_encode_test(
    nb_audio_codec_v2_opus_test_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NB_AUDIO_CODEC_SERVICE_V2_H */
