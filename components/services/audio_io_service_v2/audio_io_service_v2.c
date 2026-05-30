/*
 * audio_io_service_v2.c - inactive Audio I/O v2 skeleton.
 */

#include "audio_io_service_v2.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <string.h>

#define PROBE_MIN_DURATION_MS  16U
#define PROBE_MAX_DURATION_MS  5000U

static nb_audio_io_v2_status_t s_status;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t isqrt_u64(uint64_t value)
{
    uint64_t bit = 1ULL << 62;
    uint64_t result = 0;

    while (bit > value) {
        bit >>= 2;
    }

    while (bit != 0ULL) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }

    return (uint32_t)result;
}

static void read_heap_kb(uint32_t *internal_kb, uint32_t *dma_kb)
{
    if (internal_kb != NULL) {
        *internal_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U);
    }
    if (dma_kb != NULL) {
        *dma_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024U);
    }
}

esp_err_t audio_io_service_v2_init(void)
{
    uint32_t internal_kb = 0;
    uint32_t dma_kb = 0;
    read_heap_kb(&internal_kb, &dma_kb);

    taskENTER_CRITICAL(&s_mux);
    if (s_status.initialized) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = true;
    s_status.last_error = ESP_OK;
    s_status.heap_internal_free_kb = internal_kb;
    s_status.heap_dma_free_kb = dma_kb;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t audio_io_service_v2_deinit(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (!s_status.initialized) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_status, 0, sizeof(s_status));
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

bool audio_io_service_v2_is_initialized(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool initialized = s_status.initialized;
    taskEXIT_CRITICAL(&s_mux);
    return initialized;
}

void audio_io_service_v2_get_status(nb_audio_io_v2_status_t *out)
{
    if (out == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_mux);
    *out = s_status;
    taskEXIT_CRITICAL(&s_mux);
}

esp_err_t audio_io_service_v2_probe_start(uint32_t duration_ms)
{
    if (duration_ms < PROBE_MIN_DURATION_MS || duration_ms > PROBE_MAX_DURATION_MS) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t internal_kb = 0;
    uint32_t dma_kb = 0;
    read_heap_kb(&internal_kb, &dma_kb);

    taskENTER_CRITICAL(&s_mux);
    if (!s_status.initialized) {
        memset(&s_status, 0, sizeof(s_status));
        s_status.initialized = true;
    }
    if (s_status.probe_running) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    s_status.probe_running = true;
    s_status.probe_duration_ms = duration_ms;
    s_status.probe_elapsed_ms = 0;
    s_status.rx_frames = 0;
    s_status.tx_frames = 0;
    s_status.tx_silence_frames = 0;
    s_status.i2s_recoveries = 0;
    s_status.dropped_frames = 0;
    s_status.rms_last = 0;
    s_status.peak_last = 0;
    s_status.rms_max = 0;
    s_status.peak_max = 0;
    s_status.last_error = ESP_OK;
    s_status.heap_internal_free_kb = internal_kb;
    s_status.heap_dma_free_kb = dma_kb;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t audio_io_service_v2_probe_stop(void)
{
    uint32_t internal_kb = 0;
    uint32_t dma_kb = 0;
    read_heap_kb(&internal_kb, &dma_kb);

    taskENTER_CRITICAL(&s_mux);
    if (!s_status.probe_running) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    s_status.probe_running = false;
    s_status.heap_internal_free_kb = internal_kb;
    s_status.heap_dma_free_kb = dma_kb;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

bool audio_io_service_v2_probe_is_running(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool running = s_status.probe_running;
    taskEXIT_CRITICAL(&s_mux);
    return running;
}

void audio_io_service_v2_probe_feed_rx_frame(const int16_t *samples, uint16_t sample_count)
{
    if (samples == NULL || sample_count == 0U) {
        return;
    }

    taskENTER_CRITICAL(&s_mux);
    bool running = s_status.probe_running;
    taskEXIT_CRITICAL(&s_mux);
    if (!running) {
        return;
    }

    uint64_t sum_sq = 0;
    uint32_t peak = 0;
    for (uint16_t i = 0; i < sample_count; i++) {
        int32_t v = samples[i];
        uint32_t mag = (uint32_t)((v < 0) ? -v : v);
        sum_sq += (uint64_t)mag * (uint64_t)mag;
        if (mag > peak) {
            peak = mag;
        }
    }

    uint32_t rms = isqrt_u64(sum_sq / sample_count);

    taskENTER_CRITICAL(&s_mux);
    if (!s_status.probe_running) {
        taskEXIT_CRITICAL(&s_mux);
        return;
    }
    s_status.rx_frames++;
    s_status.rms_last = rms;
    s_status.peak_last = peak;
    if (rms > s_status.rms_max) {
        s_status.rms_max = rms;
    }
    if (peak > s_status.peak_max) {
        s_status.peak_max = peak;
    }

    s_status.probe_elapsed_ms += NB_AUDIO_IO_V2_CHUNK_MS;
    if (s_status.probe_elapsed_ms >= s_status.probe_duration_ms) {
        s_status.probe_running = false;
    }
    taskEXIT_CRITICAL(&s_mux);
}

void audio_io_service_v2_probe_note_tx_silence(esp_err_t result)
{
    taskENTER_CRITICAL(&s_mux);
    if (!s_status.probe_running) {
        taskEXIT_CRITICAL(&s_mux);
        return;
    }

    if (result == ESP_OK) {
        s_status.tx_frames++;
        s_status.tx_silence_frames++;
    } else {
        s_status.dropped_frames++;
        s_status.last_error = result;
    }
    taskEXIT_CRITICAL(&s_mux);
}
