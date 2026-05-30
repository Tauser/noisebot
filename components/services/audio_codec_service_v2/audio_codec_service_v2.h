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

typedef enum {
    NB_AUDIO_CODEC_V2_FORMAT_PCM16 = 0,
    NB_AUDIO_CODEC_V2_FORMAT_OPUS,
} nb_audio_codec_v2_format_t;

typedef struct {
    bool initialized;
    nb_audio_codec_v2_format_t format;
    uint32_t pcm_frames_in;
    uint32_t packets_out;
    uint32_t packet_drops;
    uint32_t queue_count;
    uint16_t pending_samples;
} nb_audio_codec_v2_status_t;

esp_err_t audio_codec_service_v2_init(void);
esp_err_t audio_codec_service_v2_deinit(void);
bool audio_codec_service_v2_is_initialized(void);
void audio_codec_service_v2_get_status(nb_audio_codec_v2_status_t *out);
esp_err_t audio_codec_service_v2_feed_pcm16(const int16_t *samples, uint16_t sample_count);
esp_err_t audio_codec_service_v2_encode_test_once(void);
esp_err_t audio_codec_service_v2_drain_synthetic(uint32_t *drained_packets);

#ifdef __cplusplus
}
#endif

#endif /* NB_AUDIO_CODEC_SERVICE_V2_H */
