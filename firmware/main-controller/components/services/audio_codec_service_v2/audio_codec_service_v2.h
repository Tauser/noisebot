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
#define NB_AUDIO_CODEC_V2_MAX_EGRESS_PACKETS 40U
#define NB_AUDIO_CODEC_V2_OPUS_PACKET_MAX_BYTES 1024U
#define NB_AUDIO_CODEC_V2_PAYLOAD_PREVIEW_BYTES 16U
#define NB_AUDIO_CODEC_V2_OVERFLOW_TEST_MAX_PACKETS 200U
#define NB_AUDIO_CODEC_V2_WORKER_STRESS_MAX_PACKETS NB_AUDIO_CODEC_V2_MAX_QUEUE_PACKETS

typedef enum {
    NB_AUDIO_CODEC_V2_FORMAT_PCM16 = 0,
    NB_AUDIO_CODEC_V2_FORMAT_OPUS,
} nb_audio_codec_v2_format_t;

typedef enum {
    NB_AUDIO_CODEC_V2_WORKER_STATE_NOT_STARTED = 0,
    NB_AUDIO_CODEC_V2_WORKER_STATE_STARTING,
    NB_AUDIO_CODEC_V2_WORKER_STATE_RUNNING,
    NB_AUDIO_CODEC_V2_WORKER_STATE_STOPPING,
    NB_AUDIO_CODEC_V2_WORKER_STATE_STOPPED,
    NB_AUDIO_CODEC_V2_WORKER_STATE_ERROR,
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
    uint32_t worker_drained_packets;
    uint32_t worker_opus_packets;
    uint32_t worker_opus_encoded_bytes_total;
    uint16_t worker_opus_last_packet_bytes;
    uint32_t worker_payload_packets;
    uint32_t worker_payload_bytes_total;
    uint16_t worker_payload_last_bytes;
    uint32_t worker_payload_last_sequence;
    uint32_t worker_payload_last_checksum;
    uint8_t worker_payload_preview_len;
    uint8_t worker_payload_preview[NB_AUDIO_CODEC_V2_PAYLOAD_PREVIEW_BYTES];
    uint32_t opus_egress_packets_in;
    uint32_t opus_egress_packets_drained;
    uint32_t opus_egress_packet_drops;
    uint32_t opus_egress_queue_count;
    uint32_t opus_egress_bytes_total;
    uint32_t opus_egress_last_sequence;
    uint16_t opus_egress_last_bytes;
    uint32_t opus_egress_last_checksum;
    uint8_t opus_egress_preview_len;
    uint8_t opus_egress_preview[NB_AUDIO_CODEC_V2_PAYLOAD_PREVIEW_BYTES];
    bool bridge_handoff_supported;
    bool bridge_handoff_active;
    uint32_t bridge_handoff_packets_ready;
    uint32_t bridge_handoff_bytes_ready;
    uint32_t bridge_handoff_last_sequence;
    uint16_t bridge_handoff_last_bytes;
    uint32_t bridge_handoff_last_checksum;
    uint8_t bridge_handoff_preview_len;
    uint8_t bridge_handoff_preview[NB_AUDIO_CODEC_V2_PAYLOAD_PREVIEW_BYTES];
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

typedef struct {
    uint32_t attempted_packets;
    uint32_t accepted_packets;
    uint32_t worker_drained_packets_delta;
    uint32_t worker_opus_packets_delta;
    uint32_t worker_opus_encoded_bytes_delta;
    uint16_t worker_opus_last_packet_bytes;
    uint32_t packet_drops_delta;
    uint32_t queue_count_after;
    nb_audio_codec_v2_worker_state_t worker_state_after;
} nb_audio_codec_v2_worker_stress_result_t;

typedef struct {
    uint32_t attempted_frames;
    uint32_t attempted_samples;
    uint32_t pcm_frames_in_delta;
    uint32_t packets_out_delta;
    uint32_t worker_drained_packets_delta;
    uint32_t worker_opus_packets_delta;
    uint32_t worker_opus_encoded_bytes_delta;
    uint16_t worker_opus_last_packet_bytes;
    uint32_t worker_payload_packets_delta;
    uint32_t worker_payload_bytes_delta;
    uint16_t worker_payload_last_bytes;
    uint32_t worker_payload_last_sequence;
    uint32_t worker_payload_last_checksum;
    uint8_t worker_payload_preview_len;
    uint8_t worker_payload_preview[NB_AUDIO_CODEC_V2_PAYLOAD_PREVIEW_BYTES];
    uint32_t opus_egress_packets_delta;
    uint32_t opus_egress_bytes_delta;
    uint32_t opus_egress_packet_drops_delta;
    uint32_t opus_egress_drained_after_test;
    uint32_t opus_egress_queue_count_after_cleanup;
    uint16_t opus_egress_last_bytes;
    uint32_t opus_egress_last_sequence;
    uint32_t opus_egress_last_checksum;
    uint8_t opus_egress_preview_len;
    uint8_t opus_egress_preview[NB_AUDIO_CODEC_V2_PAYLOAD_PREVIEW_BYTES];
    uint32_t packet_drops_delta;
    uint32_t queue_count_after;
    uint16_t pending_samples_after;
    nb_audio_codec_v2_worker_state_t worker_state_after;
} nb_audio_codec_v2_worker_feed_result_t;

typedef struct {
    uint32_t attempted_frames;
    bool bridge_handoff_stub;
    bool bridge_transport_unchanged;
    bool bridge_packet_not_sent;
    uint32_t opus_egress_packets_delta;
    uint32_t opus_egress_bytes_delta;
    uint32_t bridge_handoff_packets_ready_delta;
    uint32_t bridge_handoff_bytes_ready_delta;
    uint16_t bridge_handoff_last_bytes;
    uint32_t bridge_handoff_last_sequence;
    uint32_t bridge_handoff_last_checksum;
    uint8_t bridge_handoff_preview_len;
    uint8_t bridge_handoff_preview[NB_AUDIO_CODEC_V2_PAYLOAD_PREVIEW_BYTES];
    uint32_t opus_egress_queue_count_after_cleanup;
    uint32_t packet_drops_delta;
    nb_audio_codec_v2_worker_state_t worker_state_after;
} nb_audio_codec_v2_bridge_handoff_result_t;

esp_err_t audio_codec_service_v2_init(void);
esp_err_t audio_codec_service_v2_deinit(void);
bool audio_codec_service_v2_is_initialized(void);
const char *audio_codec_service_v2_worker_state_name(nb_audio_codec_v2_worker_state_t state);
void audio_codec_service_v2_get_status(nb_audio_codec_v2_status_t *out);
esp_err_t audio_codec_service_v2_feed_pcm16(const int16_t *samples, uint16_t sample_count);
esp_err_t audio_codec_service_v2_read_opus_packet(
    uint8_t *out,
    uint16_t max_len,
    uint16_t *out_len);
esp_err_t audio_codec_service_v2_encode_test_once(void);
esp_err_t audio_codec_service_v2_drain_synthetic(uint32_t *drained_packets);
esp_err_t audio_codec_service_v2_drain_opus_egress(uint32_t *drained_packets);
esp_err_t audio_codec_service_v2_reset_diagnostics(void);
esp_err_t audio_codec_service_v2_overflow_test(
    uint32_t packets,
    nb_audio_codec_v2_overflow_test_result_t *out);
esp_err_t audio_codec_service_v2_opus_encode_test(
    nb_audio_codec_v2_opus_test_result_t *out);
esp_err_t audio_codec_service_v2_worker_start(void);
esp_err_t audio_codec_service_v2_worker_stop(void);
esp_err_t audio_codec_service_v2_worker_stress_test(
    uint32_t packets,
    nb_audio_codec_v2_worker_stress_result_t *out);
esp_err_t audio_codec_service_v2_worker_feed_test(
    uint32_t frames,
    nb_audio_codec_v2_worker_feed_result_t *out);
esp_err_t audio_codec_service_v2_bridge_handoff_test(
    uint32_t frames,
    nb_audio_codec_v2_bridge_handoff_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NB_AUDIO_CODEC_SERVICE_V2_H */
