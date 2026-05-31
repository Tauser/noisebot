/*
 * audio_codec_service_v2.c - inactive Codec v2 skeleton.
 */

#include "audio_codec_service_v2.h"
#include "esp_audio_enc.h"
#include "esp_audio_types.h"
#include "esp_heap_caps.h"
#include "esp_opus_enc.h"
#include <string.h>

#define OPUS_TEST_MAX_BYTES 1024U
#define OPUS_ENC_CONFIG() {                                      \
        .sample_rate        = ESP_AUDIO_SAMPLE_RATE_16K,         \
        .channel            = ESP_AUDIO_MONO,                    \
        .bits_per_sample    = ESP_AUDIO_BIT16,                   \
        .bitrate            = NB_AUDIO_CODEC_V2_OPUS_BITRATE,    \
        .frame_duration     = ESP_OPUS_ENC_FRAME_DURATION_60_MS, \
        .application_mode   = ESP_OPUS_ENC_APPLICATION_AUDIO,    \
        .complexity         = 0,                                 \
        .enable_fec         = false,                             \
        .enable_dtx         = true,                              \
        .enable_vbr         = true,                              \
    }

static nb_audio_codec_v2_status_t s_status = {
    .format = NB_AUDIO_CODEC_V2_FORMAT_PCM16,
    .opus_codec_error = -1,
};
static int16_t s_pending_frame[NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES];
static uint16_t s_pending_samples;
static int16_t s_opus_test_frame[NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES];
static uint8_t s_opus_test_out[OPUS_TEST_MAX_BYTES];

static uint32_t heap_free_kb(uint32_t caps)
{
    return (uint32_t)(heap_caps_get_free_size(caps) / 1024U);
}

static void reset_worker_stub_status(void)
{
    s_status.worker_state = NB_AUDIO_CODEC_V2_WORKER_STATE_NOT_STARTED;
    s_status.worker_supported = false;
    s_status.worker_active = false;
}

static void fill_opus_test_frame(void)
{
    for (uint16_t i = 0; i < NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES; i++) {
        s_opus_test_frame[i] = (int16_t)(((int32_t)(i & 0x3fU) * 64) - 2048);
    }
}

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
    s_status.opus_codec_error = -1;
    reset_worker_stub_status();
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
    s_status.opus_codec_error = -1;
    reset_worker_stub_status();
    return ESP_OK;
}

bool audio_codec_service_v2_is_initialized(void)
{
    return s_status.initialized;
}

const char *audio_codec_service_v2_worker_state_name(nb_audio_codec_v2_worker_state_t state)
{
    switch (state) {
    case NB_AUDIO_CODEC_V2_WORKER_STATE_NOT_STARTED:
        return "not_started";
    default:
        return "unknown";
    }
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
    s_status.opus_codec_error = -1;
    reset_worker_stub_status();
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

esp_err_t audio_codec_service_v2_opus_encode_test(
    nb_audio_codec_v2_opus_test_result_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->codec_error = -1;
    out->internal_before_kb = heap_free_kb(MALLOC_CAP_INTERNAL);
    out->dma_before_kb = heap_free_kb(MALLOC_CAP_DMA);
    out->psram_before_kb = heap_free_kb(MALLOC_CAP_SPIRAM);

    void *enc = NULL;
    esp_opus_enc_config_t cfg = OPUS_ENC_CONFIG();
    esp_audio_err_t codec_err = esp_opus_enc_open(&cfg, sizeof(cfg), &enc);
    out->codec_error = (int)codec_err;
    out->internal_after_open_kb = heap_free_kb(MALLOC_CAP_INTERNAL);
    out->dma_after_open_kb = heap_free_kb(MALLOC_CAP_DMA);
    out->psram_after_open_kb = heap_free_kb(MALLOC_CAP_SPIRAM);

    if (codec_err != ESP_AUDIO_ERR_OK || enc == NULL) {
        s_status.opus_codec_error = (int)codec_err;
        if (enc != NULL) {
            esp_opus_enc_close(enc);
        }
        out->internal_after_close_kb = heap_free_kb(MALLOC_CAP_INTERNAL);
        out->dma_after_close_kb = heap_free_kb(MALLOC_CAP_DMA);
        out->psram_after_close_kb = heap_free_kb(MALLOC_CAP_SPIRAM);
        return ESP_ERR_NO_MEM;
    }

    int frame_bytes = 0;
    int out_bytes = 0;
    codec_err = esp_opus_enc_get_frame_size(enc, &frame_bytes, &out_bytes);
    out->codec_error = (int)codec_err;
    out->frame_samples = (uint16_t)(frame_bytes / (int)sizeof(int16_t));
    out->outbuf_bytes = (uint16_t)out_bytes;

    if (codec_err == ESP_AUDIO_ERR_OK &&
        out->frame_samples == NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES &&
        out_bytes > 0 &&
        out_bytes <= (int)sizeof(s_opus_test_out)) {
        fill_opus_test_frame();
        esp_audio_enc_in_frame_t in = {
            .buffer = (uint8_t *)s_opus_test_frame,
            .len = (uint32_t)frame_bytes,
        };
        esp_audio_enc_out_frame_t encoded = {
            .buffer = s_opus_test_out,
            .len = (uint32_t)out_bytes,
            .encoded_bytes = 0,
        };

        codec_err = esp_opus_enc_process(enc, &in, &encoded);
        out->codec_error = (int)codec_err;
        out->encoded_bytes = (uint16_t)encoded.encoded_bytes;
    }

    esp_opus_enc_close(enc);
    out->internal_after_close_kb = heap_free_kb(MALLOC_CAP_INTERNAL);
    out->dma_after_close_kb = heap_free_kb(MALLOC_CAP_DMA);
    out->psram_after_close_kb = heap_free_kb(MALLOC_CAP_SPIRAM);

    s_status.opus_codec_error = out->codec_error;
    if (codec_err != ESP_AUDIO_ERR_OK ||
        out->frame_samples != NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES ||
        out->encoded_bytes == 0U) {
        return ESP_FAIL;
    }

    s_status.pcm_frames_in++;
    s_status.packets_out++;
    s_status.opus_encode_tests++;
    s_status.opus_last_packet_bytes = out->encoded_bytes;
    s_status.opus_encoded_bytes_total += out->encoded_bytes;
    return ESP_OK;
}
