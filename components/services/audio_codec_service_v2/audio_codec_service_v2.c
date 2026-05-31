/*
 * audio_codec_service_v2.c - inactive Codec v2 skeleton.
 */

#include "audio_codec_service_v2.h"
#include "esp_audio_enc.h"
#include "esp_audio_types.h"
#include "esp_heap_caps.h"
#include "esp_opus_enc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

#define OPUS_TEST_MAX_BYTES NB_AUDIO_CODEC_V2_OPUS_PACKET_MAX_BYTES
#define OPUS_TEST_TASK_STACK (2048U * 12U)
#define OPUS_TEST_TASK_PRIORITY 2U
#define OPUS_TEST_TASK_CORE 0
#define OPUS_TEST_TIMEOUT_MS 8000U
#define CODEC_WORKER_TASK_STACK OPUS_TEST_TASK_STACK
#define CODEC_WORKER_TASK_PRIORITY 2U
#define CODEC_WORKER_TASK_CORE 0
#define CODEC_WORKER_STOP_TIMEOUT_MS 2000U
#define CODEC_WORKER_POLL_MS 20U
#define CODEC_WORKER_STRESS_TIMEOUT_MS 5000U
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
    .worker_supported = true,
    .worker_state = NB_AUDIO_CODEC_V2_WORKER_STATE_NOT_STARTED,
};
static int16_t s_pending_frame[NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES];
static uint16_t s_pending_samples;
static int16_t (*s_pcm_queue)[NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES];
static uint8_t (*s_opus_egress_packets)[NB_AUDIO_CODEC_V2_OPUS_PACKET_MAX_BYTES];
static uint16_t s_opus_egress_lengths[NB_AUDIO_CODEC_V2_MAX_EGRESS_PACKETS];
static uint8_t s_pcm_queue_read;
static uint8_t s_pcm_queue_write;
static uint8_t s_pcm_queue_count;
static uint8_t s_opus_egress_read;
static uint8_t s_opus_egress_write;
static uint8_t s_opus_egress_count;
static int16_t s_opus_test_frame[NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES];
static uint8_t s_opus_test_out[OPUS_TEST_MAX_BYTES];
static int16_t s_worker_opus_frame[NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES];
static uint8_t s_worker_opus_out[OPUS_TEST_MAX_BYTES];
static StaticSemaphore_t s_opus_test_done_buf;
static SemaphoreHandle_t s_opus_test_done;
static TaskHandle_t s_opus_test_task;
static nb_audio_codec_v2_opus_test_result_t s_opus_test_result;
static esp_err_t s_opus_test_err = ESP_ERR_INVALID_STATE;
static StaticSemaphore_t s_worker_done_buf;
static SemaphoreHandle_t s_worker_done;
static TaskHandle_t s_worker_task;
static volatile bool s_worker_stop_requested;

static uint32_t heap_free_kb(uint32_t caps)
{
    return (uint32_t)(heap_caps_get_free_size(caps) / 1024U);
}

static esp_err_t ensure_packet_storage(void)
{
    if (s_pcm_queue == NULL) {
        s_pcm_queue = heap_caps_calloc(
            NB_AUDIO_CODEC_V2_MAX_QUEUE_PACKETS,
            sizeof(*s_pcm_queue),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_pcm_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_opus_egress_packets == NULL) {
        s_opus_egress_packets = heap_caps_calloc(
            NB_AUDIO_CODEC_V2_MAX_EGRESS_PACKETS,
            sizeof(*s_opus_egress_packets),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_opus_egress_packets == NULL) {
            heap_caps_free(s_pcm_queue);
            s_pcm_queue = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

static void reset_worker_status(void)
{
    s_status.worker_state = NB_AUDIO_CODEC_V2_WORKER_STATE_NOT_STARTED;
    s_status.worker_supported = true;
    s_status.worker_active = false;
}

static void fill_synthetic_opus_frame(int16_t *frame)
{
    for (uint16_t i = 0; i < NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES; i++) {
        frame[i] = (int16_t)(((int32_t)(i & 0x3fU) * 64) - 2048);
    }
}

static void clear_pcm_queue(void)
{
    s_pcm_queue_read = 0;
    s_pcm_queue_write = 0;
    s_pcm_queue_count = 0;
    s_status.queue_count = 0;
}

static void clear_opus_egress_queue(void)
{
    s_opus_egress_read = 0;
    s_opus_egress_write = 0;
    s_opus_egress_count = 0;
    memset(s_opus_egress_lengths, 0, sizeof(s_opus_egress_lengths));
    s_status.opus_egress_queue_count = 0;
}

static esp_err_t enqueue_pcm_frame(const int16_t *frame)
{
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensure_packet_storage();
    if (err != ESP_OK) {
        return err;
    }

    if (s_pcm_queue_count >= NB_AUDIO_CODEC_V2_MAX_QUEUE_PACKETS) {
        s_status.packet_drops++;
        return ESP_OK;
    }

    memcpy(s_pcm_queue[s_pcm_queue_write],
           frame,
           NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES * sizeof(int16_t));
    s_pcm_queue_write = (uint8_t)((s_pcm_queue_write + 1U) % NB_AUDIO_CODEC_V2_MAX_QUEUE_PACKETS);
    s_pcm_queue_count++;
    s_status.queue_count = s_pcm_queue_count;
    s_status.packets_out++;
    return ESP_OK;
}

static bool pop_pcm_frame(int16_t *frame)
{
    if (frame == NULL || s_pcm_queue == NULL || s_pcm_queue_count == 0U) {
        return false;
    }

    memcpy(frame,
           s_pcm_queue[s_pcm_queue_read],
           NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES * sizeof(int16_t));
    s_pcm_queue_read = (uint8_t)((s_pcm_queue_read + 1U) % NB_AUDIO_CODEC_V2_MAX_QUEUE_PACKETS);
    s_pcm_queue_count--;
    s_status.queue_count = s_pcm_queue_count;
    return true;
}

static uint32_t checksum_payload(const uint8_t *payload, uint16_t payload_bytes)
{
    uint32_t hash = 2166136261UL;

    for (uint16_t i = 0; i < payload_bytes; i++) {
        hash ^= payload[i];
        hash *= 16777619UL;
    }

    return hash;
}

static void observe_worker_payload(const uint8_t *payload, uint16_t payload_bytes)
{
    if (payload == NULL || payload_bytes == 0U) {
        return;
    }

    s_status.worker_payload_packets++;
    s_status.worker_payload_bytes_total += payload_bytes;
    s_status.worker_payload_last_bytes = payload_bytes;
    s_status.worker_payload_last_sequence++;
    s_status.worker_payload_last_checksum = checksum_payload(payload, payload_bytes);
    s_status.worker_payload_preview_len = payload_bytes < NB_AUDIO_CODEC_V2_PAYLOAD_PREVIEW_BYTES
                                              ? (uint8_t)payload_bytes
                                              : (uint8_t)NB_AUDIO_CODEC_V2_PAYLOAD_PREVIEW_BYTES;
    memcpy(s_status.worker_payload_preview, payload, s_status.worker_payload_preview_len);
}

static void enqueue_opus_egress_packet(const uint8_t *payload, uint16_t payload_bytes)
{
    if (payload == NULL || payload_bytes == 0U) {
        return;
    }

    esp_err_t err = ensure_packet_storage();
    if (err != ESP_OK) {
        s_status.opus_egress_packet_drops++;
        return;
    }

    if (payload_bytes > NB_AUDIO_CODEC_V2_OPUS_PACKET_MAX_BYTES) {
        s_status.opus_egress_packet_drops++;
        return;
    }

    if (s_opus_egress_count >= NB_AUDIO_CODEC_V2_MAX_EGRESS_PACKETS) {
        s_status.opus_egress_packet_drops++;
        return;
    }

    memcpy(s_opus_egress_packets[s_opus_egress_write], payload, payload_bytes);
    s_opus_egress_lengths[s_opus_egress_write] = payload_bytes;
    s_opus_egress_write = (uint8_t)((s_opus_egress_write + 1U) % NB_AUDIO_CODEC_V2_MAX_EGRESS_PACKETS);
    s_opus_egress_count++;
    s_status.opus_egress_queue_count = s_opus_egress_count;
    s_status.opus_egress_packets_in++;
    s_status.opus_egress_bytes_total += payload_bytes;
    s_status.opus_egress_last_sequence++;
    s_status.opus_egress_last_bytes = payload_bytes;
    s_status.opus_egress_last_checksum = checksum_payload(payload, payload_bytes);
    s_status.opus_egress_preview_len = payload_bytes < NB_AUDIO_CODEC_V2_PAYLOAD_PREVIEW_BYTES
                                            ? (uint8_t)payload_bytes
                                            : (uint8_t)NB_AUDIO_CODEC_V2_PAYLOAD_PREVIEW_BYTES;
    memcpy(s_status.opus_egress_preview, payload, s_status.opus_egress_preview_len);
}

static esp_err_t encode_opus_frame(
    void *enc,
    int16_t *frame,
    uint8_t *out_buf,
    uint16_t *encoded_bytes)
{
    if (enc == NULL || frame == NULL || out_buf == NULL || encoded_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int frame_bytes = 0;
    int out_bytes = 0;
    esp_audio_err_t codec_err = esp_opus_enc_get_frame_size(enc, &frame_bytes, &out_bytes);
    s_status.opus_codec_error = (int)codec_err;
    if (codec_err != ESP_AUDIO_ERR_OK ||
        frame_bytes != (int)(NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES * sizeof(int16_t)) ||
        out_bytes <= 0 ||
        out_bytes > OPUS_TEST_MAX_BYTES) {
        return ESP_FAIL;
    }

    esp_audio_enc_in_frame_t in = {
        .buffer = (uint8_t *)frame,
        .len = (uint32_t)frame_bytes,
    };
    esp_audio_enc_out_frame_t encoded = {
        .buffer = out_buf,
        .len = (uint32_t)out_bytes,
        .encoded_bytes = 0,
    };

    codec_err = esp_opus_enc_process(enc, &in, &encoded);
    s_status.opus_codec_error = (int)codec_err;
    if (codec_err != ESP_AUDIO_ERR_OK || encoded.encoded_bytes == 0U) {
        return ESP_FAIL;
    }

    *encoded_bytes = (uint16_t)encoded.encoded_bytes;
    return ESP_OK;
}

static esp_err_t encode_synthetic_opus_frame(
    void *enc,
    int16_t *frame,
    uint8_t *out_buf,
    uint16_t *encoded_bytes)
{
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fill_synthetic_opus_frame(frame);
    return encode_opus_frame(enc, frame, out_buf, encoded_bytes);
}

static esp_err_t opus_encode_test_inline(nb_audio_codec_v2_opus_test_result_t *out)
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
        uint16_t encoded_bytes = 0;
        esp_err_t encode_err = encode_synthetic_opus_frame(
            enc,
            s_opus_test_frame,
            s_opus_test_out,
            &encoded_bytes);
        codec_err = (encode_err == ESP_OK) ? ESP_AUDIO_ERR_OK : (esp_audio_err_t)s_status.opus_codec_error;
        out->codec_error = (int)codec_err;
        out->encoded_bytes = encoded_bytes;
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

static void opus_encode_test_task(void *arg)
{
    (void)arg;
    s_opus_test_err = opus_encode_test_inline(&s_opus_test_result);
    s_opus_test_task = NULL;
    if (s_opus_test_done != NULL) {
        xSemaphoreGive(s_opus_test_done);
    }
    vTaskDelete(NULL);
}

static void codec_worker_task(void *arg)
{
    (void)arg;
    void *enc = NULL;
    esp_opus_enc_config_t cfg = OPUS_ENC_CONFIG();
    esp_audio_err_t codec_err = esp_opus_enc_open(&cfg, sizeof(cfg), &enc);
    s_status.opus_codec_error = (int)codec_err;
    if (codec_err != ESP_AUDIO_ERR_OK || enc == NULL) {
        if (enc != NULL) {
            esp_opus_enc_close(enc);
        }
        s_status.worker_active = false;
        s_status.worker_state = NB_AUDIO_CODEC_V2_WORKER_STATE_ERROR;
        s_worker_task = NULL;
        if (s_worker_done != NULL) {
            xSemaphoreGive(s_worker_done);
        }
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
        vTaskDeleteWithCaps(NULL);
#else
        vTaskDelete(NULL);
#endif
        return;
    }

    s_status.worker_state = NB_AUDIO_CODEC_V2_WORKER_STATE_RUNNING;
    s_status.worker_active = true;

    while (!s_worker_stop_requested) {
        while (s_pcm_queue_count > 0U && !s_worker_stop_requested) {
            if (!pop_pcm_frame(s_worker_opus_frame)) {
                break;
            }
            uint16_t encoded_bytes = 0;
            esp_err_t encode_err = encode_opus_frame(
                enc,
                s_worker_opus_frame,
                s_worker_opus_out,
                &encoded_bytes);
            if (encode_err != ESP_OK) {
                s_status.packet_drops++;
                s_status.worker_state = NB_AUDIO_CODEC_V2_WORKER_STATE_ERROR;
                s_worker_stop_requested = true;
                break;
            }
            s_status.worker_drained_packets++;
            s_status.worker_opus_packets++;
            s_status.worker_opus_last_packet_bytes = encoded_bytes;
            s_status.worker_opus_encoded_bytes_total += encoded_bytes;
            observe_worker_payload(s_worker_opus_out, encoded_bytes);
            enqueue_opus_egress_packet(s_worker_opus_out, encoded_bytes);
        }
        vTaskDelay(pdMS_TO_TICKS(CODEC_WORKER_POLL_MS));
    }

    while (s_pcm_queue_count > 0U && s_status.worker_state != NB_AUDIO_CODEC_V2_WORKER_STATE_ERROR) {
        if (!pop_pcm_frame(s_worker_opus_frame)) {
            break;
        }
        uint16_t encoded_bytes = 0;
        esp_err_t encode_err = encode_opus_frame(
            enc,
            s_worker_opus_frame,
            s_worker_opus_out,
            &encoded_bytes);
        if (encode_err != ESP_OK) {
            s_status.packet_drops += s_pcm_queue_count + 1U;
            clear_pcm_queue();
            s_status.worker_state = NB_AUDIO_CODEC_V2_WORKER_STATE_ERROR;
            break;
        }
        s_status.worker_drained_packets++;
        s_status.worker_opus_packets++;
        s_status.worker_opus_last_packet_bytes = encoded_bytes;
        s_status.worker_opus_encoded_bytes_total += encoded_bytes;
        observe_worker_payload(s_worker_opus_out, encoded_bytes);
        enqueue_opus_egress_packet(s_worker_opus_out, encoded_bytes);
    }

    esp_opus_enc_close(enc);
    s_status.worker_active = false;
    if (s_status.worker_state != NB_AUDIO_CODEC_V2_WORKER_STATE_ERROR) {
        s_status.worker_state = NB_AUDIO_CODEC_V2_WORKER_STATE_STOPPED;
    }
    s_worker_task = NULL;
    if (s_worker_done != NULL) {
        xSemaphoreGive(s_worker_done);
    }
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    vTaskDeleteWithCaps(NULL);
#else
    vTaskDelete(NULL);
#endif
}

static void enqueue_synthetic_packet(void)
{
    fill_synthetic_opus_frame(s_opus_test_frame);
    (void)enqueue_pcm_frame(s_opus_test_frame);
}

esp_err_t audio_codec_service_v2_init(void)
{
    if (s_status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_status, 0, sizeof(s_status));
    memset(s_pending_frame, 0, sizeof(s_pending_frame));
    s_pending_samples = 0;
    clear_pcm_queue();
    clear_opus_egress_queue();
    s_status.initialized = true;
    s_status.format = NB_AUDIO_CODEC_V2_FORMAT_PCM16;
    s_status.opus_codec_error = -1;
    s_status.bridge_handoff_supported = true;
    reset_worker_status();
    return ESP_OK;
}

esp_err_t audio_codec_service_v2_deinit(void)
{
    if (!s_status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_worker_task != NULL || s_status.worker_active) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_status, 0, sizeof(s_status));
    memset(s_pending_frame, 0, sizeof(s_pending_frame));
    s_pending_samples = 0;
    clear_pcm_queue();
    clear_opus_egress_queue();
    if (s_pcm_queue != NULL) {
        heap_caps_free(s_pcm_queue);
        s_pcm_queue = NULL;
    }
    if (s_opus_egress_packets != NULL) {
        heap_caps_free(s_opus_egress_packets);
        s_opus_egress_packets = NULL;
    }
    s_status.format = NB_AUDIO_CODEC_V2_FORMAT_PCM16;
    s_status.opus_codec_error = -1;
    s_status.bridge_handoff_supported = true;
    reset_worker_status();
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
    case NB_AUDIO_CODEC_V2_WORKER_STATE_STARTING:
        return "starting";
    case NB_AUDIO_CODEC_V2_WORKER_STATE_RUNNING:
        return "running";
    case NB_AUDIO_CODEC_V2_WORKER_STATE_STOPPING:
        return "stopping";
    case NB_AUDIO_CODEC_V2_WORKER_STATE_STOPPED:
        return "stopped";
    case NB_AUDIO_CODEC_V2_WORKER_STATE_ERROR:
        return "error";
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
            esp_err_t err = enqueue_pcm_frame(s_pending_frame);
            if (err != ESP_OK) {
                s_pending_samples = 0;
                return err;
            }
            s_pending_samples = 0;
        }
    }

    return ESP_OK;
}

esp_err_t audio_codec_service_v2_read_opus_packet(
    uint8_t *out,
    uint16_t max_len,
    uint16_t *out_len)
{
    if (out == NULL || out_len == NULL || max_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_opus_egress_packets == NULL || s_opus_egress_count == 0U) {
        *out_len = 0;
        return ESP_ERR_NOT_FOUND;
    }

    uint16_t packet_len = s_opus_egress_lengths[s_opus_egress_read];
    if (packet_len == 0U || packet_len > NB_AUDIO_CODEC_V2_OPUS_PACKET_MAX_BYTES) {
        s_status.opus_egress_packet_drops++;
        s_opus_egress_read = (uint8_t)((s_opus_egress_read + 1U) % NB_AUDIO_CODEC_V2_MAX_EGRESS_PACKETS);
        s_opus_egress_count--;
        s_status.opus_egress_queue_count = s_opus_egress_count;
        *out_len = 0;
        return ESP_FAIL;
    }
    if (packet_len > max_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out, s_opus_egress_packets[s_opus_egress_read], packet_len);
    s_opus_egress_lengths[s_opus_egress_read] = 0;
    s_opus_egress_read = (uint8_t)((s_opus_egress_read + 1U) % NB_AUDIO_CODEC_V2_MAX_EGRESS_PACKETS);
    s_opus_egress_count--;
    s_status.opus_egress_queue_count = s_opus_egress_count;
    s_status.opus_egress_packets_drained++;
    *out_len = packet_len;
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
    clear_pcm_queue();
    return ESP_OK;
}

esp_err_t audio_codec_service_v2_drain_opus_egress(uint32_t *drained_packets)
{
    if (drained_packets == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *drained_packets = s_opus_egress_count;
    s_status.opus_egress_packets_drained += s_opus_egress_count;
    clear_opus_egress_queue();
    return ESP_OK;
}

esp_err_t audio_codec_service_v2_reset_diagnostics(void)
{
    bool was_initialized = s_status.initialized;
    bool worker_supported = s_status.worker_supported;
    bool worker_active = s_status.worker_active;
    nb_audio_codec_v2_worker_state_t worker_state = s_status.worker_state;

    memset(&s_status, 0, sizeof(s_status));
    memset(s_pending_frame, 0, sizeof(s_pending_frame));
    s_pending_samples = 0;
    clear_pcm_queue();
    clear_opus_egress_queue();
    s_status.initialized = was_initialized;
    s_status.format = NB_AUDIO_CODEC_V2_FORMAT_PCM16;
    s_status.opus_codec_error = -1;
    s_status.bridge_handoff_supported = true;
    s_status.bridge_handoff_active = false;
    s_status.worker_supported = worker_supported;
    s_status.worker_active = worker_active;
    s_status.worker_state = worker_state;
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

    if (s_opus_test_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_opus_test_done == NULL) {
        s_opus_test_done = xSemaphoreCreateBinaryStatic(&s_opus_test_done_buf);
        if (s_opus_test_done == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    while (xSemaphoreTake(s_opus_test_done, 0) == pdTRUE) {
    }

    memset(&s_opus_test_result, 0, sizeof(s_opus_test_result));
    s_opus_test_result.codec_error = -1;
    s_opus_test_err = ESP_ERR_TIMEOUT;

    BaseType_t rc = xTaskCreatePinnedToCore(
        opus_encode_test_task,
        "nb_codec_v2_opus_test",
        OPUS_TEST_TASK_STACK,
        NULL,
        OPUS_TEST_TASK_PRIORITY,
        &s_opus_test_task,
        OPUS_TEST_TASK_CORE);
    if (rc != pdPASS) {
        s_opus_test_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_opus_test_done, pdMS_TO_TICKS(OPUS_TEST_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    *out = s_opus_test_result;
    return s_opus_test_err;
}

esp_err_t audio_codec_service_v2_worker_start(void)
{
    if (s_worker_task != NULL || s_status.worker_active) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t storage_err = ensure_packet_storage();
    if (storage_err != ESP_OK) {
        s_status.worker_state = NB_AUDIO_CODEC_V2_WORKER_STATE_ERROR;
        return storage_err;
    }

    if (s_worker_done == NULL) {
        s_worker_done = xSemaphoreCreateBinaryStatic(&s_worker_done_buf);
        if (s_worker_done == NULL) {
            s_status.worker_state = NB_AUDIO_CODEC_V2_WORKER_STATE_ERROR;
            return ESP_ERR_NO_MEM;
        }
    }
    while (xSemaphoreTake(s_worker_done, 0) == pdTRUE) {
    }

    s_worker_stop_requested = false;
    s_status.worker_supported = true;
    s_status.worker_active = false;
    s_status.worker_state = NB_AUDIO_CODEC_V2_WORKER_STATE_STARTING;

    BaseType_t rc;
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    rc = xTaskCreatePinnedToCoreWithCaps(
        codec_worker_task,
        "nb_codec_v2_worker",
        CODEC_WORKER_TASK_STACK,
        NULL,
        CODEC_WORKER_TASK_PRIORITY,
        &s_worker_task,
        CODEC_WORKER_TASK_CORE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    rc = xTaskCreatePinnedToCore(
        codec_worker_task,
        "nb_codec_v2_worker",
        CODEC_WORKER_TASK_STACK,
        NULL,
        CODEC_WORKER_TASK_PRIORITY,
        &s_worker_task,
        CODEC_WORKER_TASK_CORE);
#endif
    if (rc != pdPASS) {
        s_worker_task = NULL;
        s_status.worker_active = false;
        s_status.worker_state = NB_AUDIO_CODEC_V2_WORKER_STATE_ERROR;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t audio_codec_service_v2_worker_stop(void)
{
    if (s_worker_task == NULL && !s_status.worker_active) {
        s_status.worker_active = false;
        if (s_status.worker_state == NB_AUDIO_CODEC_V2_WORKER_STATE_STARTING ||
            s_status.worker_state == NB_AUDIO_CODEC_V2_WORKER_STATE_RUNNING ||
            s_status.worker_state == NB_AUDIO_CODEC_V2_WORKER_STATE_STOPPING) {
            s_status.worker_state = NB_AUDIO_CODEC_V2_WORKER_STATE_STOPPED;
        }
        return ESP_OK;
    }

    if (s_worker_done == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_status.worker_state = NB_AUDIO_CODEC_V2_WORKER_STATE_STOPPING;
    s_worker_stop_requested = true;

    if (xSemaphoreTake(s_worker_done, pdMS_TO_TICKS(CODEC_WORKER_STOP_TIMEOUT_MS)) != pdTRUE) {
        s_status.worker_state = NB_AUDIO_CODEC_V2_WORKER_STATE_ERROR;
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t audio_codec_service_v2_worker_stress_test(
    uint32_t packets,
    nb_audio_codec_v2_worker_stress_result_t *out)
{
    if (out == NULL ||
        packets == 0U ||
        packets > NB_AUDIO_CODEC_V2_WORKER_STRESS_MAX_PACKETS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_worker_task != NULL || s_status.worker_active) {
        return ESP_ERR_INVALID_STATE;
    }

    audio_codec_service_v2_reset_diagnostics();
    memset(out, 0, sizeof(*out));
    out->attempted_packets = packets;

    esp_err_t err = audio_codec_service_v2_worker_start();
    if (err != ESP_OK) {
        out->worker_state_after = s_status.worker_state;
        return err;
    }

    uint32_t start_wait_ms = 0;
    while (s_status.worker_state == NB_AUDIO_CODEC_V2_WORKER_STATE_STARTING &&
           start_wait_ms < CODEC_WORKER_STRESS_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(CODEC_WORKER_POLL_MS));
        start_wait_ms += CODEC_WORKER_POLL_MS;
    }
    if (s_status.worker_state != NB_AUDIO_CODEC_V2_WORKER_STATE_RUNNING) {
        (void)audio_codec_service_v2_worker_stop();
        out->worker_state_after = s_status.worker_state;
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t drained_before = s_status.worker_drained_packets;
    uint32_t opus_packets_before = s_status.worker_opus_packets;
    uint32_t opus_bytes_before = s_status.worker_opus_encoded_bytes_total;
    uint32_t drops_before = s_status.packet_drops;

    for (uint32_t i = 0; i < packets; i++) {
        enqueue_synthetic_packet();
    }
    out->accepted_packets = s_status.packets_out;

    uint32_t waited_ms = 0;
    while ((s_status.worker_drained_packets - drained_before) < out->accepted_packets &&
           s_status.worker_state == NB_AUDIO_CODEC_V2_WORKER_STATE_RUNNING &&
           waited_ms < CODEC_WORKER_STRESS_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(CODEC_WORKER_POLL_MS));
        waited_ms += CODEC_WORKER_POLL_MS;
    }

    err = ESP_OK;
    if ((s_status.worker_drained_packets - drained_before) < out->accepted_packets ||
        s_status.worker_state == NB_AUDIO_CODEC_V2_WORKER_STATE_ERROR) {
        err = ESP_ERR_TIMEOUT;
    }

    esp_err_t stop_err = audio_codec_service_v2_worker_stop();
    if (err == ESP_OK && stop_err != ESP_OK) {
        err = stop_err;
    }

    out->worker_drained_packets_delta = s_status.worker_drained_packets - drained_before;
    out->worker_opus_packets_delta = s_status.worker_opus_packets - opus_packets_before;
    out->worker_opus_encoded_bytes_delta = s_status.worker_opus_encoded_bytes_total - opus_bytes_before;
    out->worker_opus_last_packet_bytes = s_status.worker_opus_last_packet_bytes;
    out->packet_drops_delta = s_status.packet_drops - drops_before;
    out->queue_count_after = s_status.queue_count;
    out->worker_state_after = s_status.worker_state;
    return err;
}

esp_err_t audio_codec_service_v2_worker_feed_test(
    uint32_t frames,
    nb_audio_codec_v2_worker_feed_result_t *out)
{
    if (out == NULL ||
        frames == 0U ||
        frames > NB_AUDIO_CODEC_V2_WORKER_STRESS_MAX_PACKETS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_worker_task != NULL || s_status.worker_active) {
        return ESP_ERR_INVALID_STATE;
    }

    audio_codec_service_v2_reset_diagnostics();
    memset(out, 0, sizeof(*out));
    out->attempted_frames = frames;
    out->attempted_samples = frames * NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES;

    esp_err_t err = audio_codec_service_v2_worker_start();
    if (err != ESP_OK) {
        out->worker_state_after = s_status.worker_state;
        return err;
    }

    uint32_t start_wait_ms = 0;
    while (s_status.worker_state == NB_AUDIO_CODEC_V2_WORKER_STATE_STARTING &&
           start_wait_ms < CODEC_WORKER_STRESS_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(CODEC_WORKER_POLL_MS));
        start_wait_ms += CODEC_WORKER_POLL_MS;
    }
    if (s_status.worker_state != NB_AUDIO_CODEC_V2_WORKER_STATE_RUNNING) {
        (void)audio_codec_service_v2_worker_stop();
        out->worker_state_after = s_status.worker_state;
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t pcm_frames_before = s_status.pcm_frames_in;
    uint32_t packets_before = s_status.packets_out;
    uint32_t drained_before = s_status.worker_drained_packets;
    uint32_t opus_packets_before = s_status.worker_opus_packets;
    uint32_t opus_bytes_before = s_status.worker_opus_encoded_bytes_total;
    uint32_t payload_packets_before = s_status.worker_payload_packets;
    uint32_t payload_bytes_before = s_status.worker_payload_bytes_total;
    uint32_t egress_packets_before = s_status.opus_egress_packets_in;
    uint32_t egress_bytes_before = s_status.opus_egress_bytes_total;
    uint32_t egress_drops_before = s_status.opus_egress_packet_drops;
    uint32_t drops_before = s_status.packet_drops;

    for (uint32_t frame = 0; frame < frames; frame++) {
        for (uint16_t i = 0; i < NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES; i++) {
            s_opus_test_frame[i] = (int16_t)((((int32_t)frame + (int32_t)i) & 0x7f) * 32);
        }
        err = audio_codec_service_v2_feed_pcm16(
            s_opus_test_frame,
            NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES);
        if (err != ESP_OK) {
            break;
        }
    }

    out->pcm_frames_in_delta = s_status.pcm_frames_in - pcm_frames_before;
    out->packets_out_delta = s_status.packets_out - packets_before;

    uint32_t waited_ms = 0;
    while ((s_status.worker_drained_packets - drained_before) < out->packets_out_delta &&
           s_status.worker_state == NB_AUDIO_CODEC_V2_WORKER_STATE_RUNNING &&
           waited_ms < CODEC_WORKER_STRESS_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(CODEC_WORKER_POLL_MS));
        waited_ms += CODEC_WORKER_POLL_MS;
    }

    if (err == ESP_OK &&
        ((s_status.worker_drained_packets - drained_before) < out->packets_out_delta ||
         s_status.worker_state == NB_AUDIO_CODEC_V2_WORKER_STATE_ERROR)) {
        err = ESP_ERR_TIMEOUT;
    }

    esp_err_t stop_err = audio_codec_service_v2_worker_stop();
    if (err == ESP_OK && stop_err != ESP_OK) {
        err = stop_err;
    }

    out->worker_drained_packets_delta = s_status.worker_drained_packets - drained_before;
    out->worker_opus_packets_delta = s_status.worker_opus_packets - opus_packets_before;
    out->worker_opus_encoded_bytes_delta = s_status.worker_opus_encoded_bytes_total - opus_bytes_before;
    out->worker_opus_last_packet_bytes = s_status.worker_opus_last_packet_bytes;
    out->worker_payload_packets_delta = s_status.worker_payload_packets - payload_packets_before;
    out->worker_payload_bytes_delta = s_status.worker_payload_bytes_total - payload_bytes_before;
    out->worker_payload_last_bytes = s_status.worker_payload_last_bytes;
    out->worker_payload_last_sequence = s_status.worker_payload_last_sequence;
    out->worker_payload_last_checksum = s_status.worker_payload_last_checksum;
    out->worker_payload_preview_len = s_status.worker_payload_preview_len;
    memcpy(out->worker_payload_preview, s_status.worker_payload_preview, sizeof(out->worker_payload_preview));
    out->opus_egress_packets_delta = s_status.opus_egress_packets_in - egress_packets_before;
    out->opus_egress_bytes_delta = s_status.opus_egress_bytes_total - egress_bytes_before;
    out->opus_egress_packet_drops_delta = s_status.opus_egress_packet_drops - egress_drops_before;
    out->opus_egress_last_bytes = s_status.opus_egress_last_bytes;
    out->opus_egress_last_sequence = s_status.opus_egress_last_sequence;
    out->opus_egress_last_checksum = s_status.opus_egress_last_checksum;
    out->opus_egress_preview_len = s_status.opus_egress_preview_len;
    memcpy(out->opus_egress_preview, s_status.opus_egress_preview, sizeof(out->opus_egress_preview));
    (void)audio_codec_service_v2_drain_opus_egress(&out->opus_egress_drained_after_test);
    out->opus_egress_queue_count_after_cleanup = s_status.opus_egress_queue_count;
    out->packet_drops_delta = s_status.packet_drops - drops_before;
    out->queue_count_after = s_status.queue_count;
    out->pending_samples_after = s_pending_samples;
    out->worker_state_after = s_status.worker_state;
    return err;
}

esp_err_t audio_codec_service_v2_bridge_handoff_test(
    uint32_t frames,
    nb_audio_codec_v2_bridge_handoff_result_t *out)
{
    if (out == NULL ||
        frames == 0U ||
        frames > NB_AUDIO_CODEC_V2_WORKER_STRESS_MAX_PACKETS) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t handoff_packets_before = s_status.bridge_handoff_packets_ready;
    uint32_t handoff_bytes_before = s_status.bridge_handoff_bytes_ready;

    nb_audio_codec_v2_worker_feed_result_t feed;
    esp_err_t err = audio_codec_service_v2_worker_feed_test(frames, &feed);

    memset(out, 0, sizeof(*out));
    out->attempted_frames = frames;
    out->bridge_handoff_stub = true;
    out->bridge_transport_unchanged = true;
    out->bridge_packet_not_sent = true;
    out->opus_egress_packets_delta = feed.opus_egress_packets_delta;
    out->opus_egress_bytes_delta = feed.opus_egress_bytes_delta;
    out->opus_egress_queue_count_after_cleanup = feed.opus_egress_queue_count_after_cleanup;
    out->packet_drops_delta = feed.packet_drops_delta + feed.opus_egress_packet_drops_delta;
    out->worker_state_after = feed.worker_state_after;

    if (err == ESP_OK) {
        s_status.bridge_handoff_supported = true;
        s_status.bridge_handoff_active = false;
        s_status.bridge_handoff_packets_ready += feed.opus_egress_packets_delta;
        s_status.bridge_handoff_bytes_ready += feed.opus_egress_bytes_delta;
        s_status.bridge_handoff_last_sequence = feed.opus_egress_last_sequence;
        s_status.bridge_handoff_last_bytes = feed.opus_egress_last_bytes;
        s_status.bridge_handoff_last_checksum = feed.opus_egress_last_checksum;
        s_status.bridge_handoff_preview_len = feed.opus_egress_preview_len;
        memcpy(s_status.bridge_handoff_preview,
               feed.opus_egress_preview,
               sizeof(s_status.bridge_handoff_preview));
    }

    out->bridge_handoff_packets_ready_delta =
        s_status.bridge_handoff_packets_ready - handoff_packets_before;
    out->bridge_handoff_bytes_ready_delta =
        s_status.bridge_handoff_bytes_ready - handoff_bytes_before;
    out->bridge_handoff_last_bytes = s_status.bridge_handoff_last_bytes;
    out->bridge_handoff_last_sequence = s_status.bridge_handoff_last_sequence;
    out->bridge_handoff_last_checksum = s_status.bridge_handoff_last_checksum;
    out->bridge_handoff_preview_len = s_status.bridge_handoff_preview_len;
    memcpy(out->bridge_handoff_preview,
           s_status.bridge_handoff_preview,
           sizeof(out->bridge_handoff_preview));

    return err;
}
