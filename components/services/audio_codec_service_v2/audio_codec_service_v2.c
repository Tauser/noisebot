/*
 * audio_codec_service_v2.c - inactive Codec v2 skeleton.
 */

#include "audio_codec_service_v2.h"
#include <string.h>

static nb_audio_codec_v2_status_t s_status = {
    .format = NB_AUDIO_CODEC_V2_FORMAT_PCM16,
};
static int16_t s_pending_frame[NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES];
static uint16_t s_pending_samples;

static void enqueue_synthetic_packet(void)
{
    if (s_status.queue_count >= NB_AUDIO_CODEC_V2_MAX_QUEUE_PACKETS) {
        s_status.packet_drops++;
        return;
    }

    s_status.queue_count++;
    s_status.packets_out++;
}

esp_err_t audio_codec_service_v2_init(void)
{
    if (s_status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_status, 0, sizeof(s_status));
    memset(s_pending_frame, 0, sizeof(s_pending_frame));
    s_pending_samples = 0;
    s_status.initialized = true;
    s_status.format = NB_AUDIO_CODEC_V2_FORMAT_PCM16;
    return ESP_OK;
}

esp_err_t audio_codec_service_v2_deinit(void)
{
    if (!s_status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_status, 0, sizeof(s_status));
    memset(s_pending_frame, 0, sizeof(s_pending_frame));
    s_pending_samples = 0;
    s_status.format = NB_AUDIO_CODEC_V2_FORMAT_PCM16;
    return ESP_OK;
}

bool audio_codec_service_v2_is_initialized(void)
{
    return s_status.initialized;
}

void audio_codec_service_v2_get_status(nb_audio_codec_v2_status_t *out)
{
    if (out == NULL) {
        return;
    }

    *out = s_status;
    out->pending_samples = s_pending_samples;
}

esp_err_t audio_codec_service_v2_feed_pcm16(const int16_t *samples, uint16_t sample_count)
{
    if (samples == NULL || sample_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    s_status.format = NB_AUDIO_CODEC_V2_FORMAT_PCM16;

    for (uint16_t i = 0; i < sample_count; i++) {
        s_pending_frame[s_pending_samples++] = samples[i];
        if (s_pending_samples >= NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES) {
            s_status.pcm_frames_in++;
            enqueue_synthetic_packet();
            s_pending_samples = 0;
        }
    }

    return ESP_OK;
}

esp_err_t audio_codec_service_v2_encode_test_once(void)
{
    int16_t chunk[256];

    for (uint16_t i = 0; i < (uint16_t)(sizeof(chunk) / sizeof(chunk[0])); i++) {
        chunk[i] = (int16_t)(((int32_t)i % 64) * 32);
    }

    for (uint8_t i = 0; i < 4U; i++) {
        esp_err_t err = audio_codec_service_v2_feed_pcm16(chunk, (uint16_t)(sizeof(chunk) / sizeof(chunk[0])));
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t audio_codec_service_v2_drain_synthetic(uint32_t *drained_packets)
{
    if (drained_packets == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *drained_packets = s_status.queue_count;
    s_status.queue_count = 0;
    return ESP_OK;
}

esp_err_t audio_codec_service_v2_reset_diagnostics(void)
{
    bool was_initialized = s_status.initialized;

    memset(&s_status, 0, sizeof(s_status));
    memset(s_pending_frame, 0, sizeof(s_pending_frame));
    s_pending_samples = 0;
    s_status.initialized = was_initialized;
    s_status.format = NB_AUDIO_CODEC_V2_FORMAT_PCM16;
    return ESP_OK;
}

esp_err_t audio_codec_service_v2_overflow_test(
    uint32_t packets,
    nb_audio_codec_v2_overflow_test_result_t *out)
{
    if (out == NULL || packets == 0U || packets > NB_AUDIO_CODEC_V2_OVERFLOW_TEST_MAX_PACKETS) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_codec_service_v2_reset_diagnostics();
    memset(out, 0, sizeof(*out));
    out->attempted_packets = packets;

    for (uint32_t i = 0; i < packets; i++) {
        enqueue_synthetic_packet();
        if (s_status.queue_count > out->peak_queue_count) {
            out->peak_queue_count = s_status.queue_count;
        }
    }

    out->accepted_packets = s_status.packets_out;
    out->dropped_packets = s_status.packet_drops;

    audio_codec_service_v2_reset_diagnostics();
    out->queue_count_after_cleanup = s_status.queue_count;
    out->status_packet_drops_after_cleanup = s_status.packet_drops;
    return ESP_OK;
}
