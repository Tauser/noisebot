/*
 * audio_processor_service.c — Probe experimental de AFE de voz (Layer 4)
 */

#include "audio_processor_service.h"
#include "board_caps.h"

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "model_path.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <math.h>
#include <string.h>

#define TAG "audio_proc"

#define NVS_NS_SERVICE          "nb_svc"
#define NVS_KEY_VOICE_AFE_PROBE "voice_afe_probe"
#define SHADOW_TASK_STACK       4096U
#define SHADOW_TASK_PRIORITY    5U
#define SHADOW_TASK_CORE        0
#define FEED_BUF_MAX            512U
#define PROCESSED_RING_SAMPLES  8192U
#define SHADOW_LOG_FETCH_CHUNKS 250U
#define AFE_MIN_INTERNAL_FREE_KB 96U
#define AFE_MIN_DMA_LARGEST_KB   48U

typedef struct {
    nb_audio_processor_status_t status;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_buf;
    const esp_afe_sr_iface_t *handle;
    esp_afe_sr_data_t *data;
    TaskHandle_t task;
    int16_t feed_buf[FEED_BUF_MAX];
    uint16_t feed_pos;
    int16_t *processed_ring;
    uint16_t processed_ring_capacity;
    uint16_t processed_ring_read;
    uint16_t processed_ring_write;
    uint16_t processed_ring_count;
} audio_processor_state_t;

static audio_processor_state_t s;

static uint32_t psram_free_kb(void)
{
    return (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U);
}

static uint32_t internal_free_kb(void)
{
    return (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U);
}

static uint32_t internal_largest_kb(void)
{
    return (uint32_t)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024U);
}

static uint32_t dma_free_kb(void)
{
    return (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024U);
}

static uint32_t dma_largest_kb(void)
{
    return (uint32_t)(heap_caps_get_largest_free_block(MALLOC_CAP_DMA) / 1024U);
}

static void update_heap_status_locked(void)
{
    s.status.internal_free_kb = internal_free_kb();
    s.status.internal_largest_kb = internal_largest_kb();
    s.status.dma_free_kb = dma_free_kb();
    s.status.dma_largest_kb = dma_largest_kb();
}

static bool afe_runtime_heap_ok(void)
{
    return internal_free_kb() >= AFE_MIN_INTERNAL_FREE_KB &&
           dma_largest_kb() >= AFE_MIN_DMA_LARGEST_KB;
}

static void update_board_caps_locked(void)
{
    const nb_board_caps_t *caps = nb_board_caps_get();
    bool supported = caps != NULL && caps->supports_device_aec;
    s.status.aec_supported = supported;
    s.status.aec_blocked_no_reference = !supported;
}

static bool probe_enabled_from_nvs(void)
{
    bool enabled = false;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_SERVICE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t value = 0;
        if (nvs_get_u8(nvs, NVS_KEY_VOICE_AFE_PROBE, &value) == ESP_OK) {
            enabled = (value != 0U);
        }
        nvs_close(nvs);
    }
    return enabled;
}

static esp_err_t create_afe_instance(const char *input_format,
                                     afe_type_t type,
                                     afe_mode_t mode,
                                     bool enable_aec,
                                     const esp_afe_sr_iface_t **handle_out,
                                     esp_afe_sr_data_t **data_out)
{
    srmodel_list_t *models = NULL;
    afe_config_t *cfg = NULL;
    const esp_afe_sr_iface_t *handle = NULL;
    esp_afe_sr_data_t *data = NULL;
    esp_err_t err = ESP_FAIL;

    models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGE(TAG, "esp_srmodel_init falhou — partição 'model' indisponivel");
        err = ESP_FAIL;
        goto done;
    }

    cfg = afe_config_init(input_format, models, type, mode);
    if (cfg == NULL) {
        ESP_LOGE(TAG, "afe_config_init(%s) falhou", input_format);
        err = ESP_FAIL;
        goto done;
    }

    /* SE fica desligado no hardware mono. O AEC só entra nos probes dedicados;
     * o runtime principal continua opt-in e protegido por margem de heap. */
    cfg->aec_init = enable_aec;
    cfg->se_init = false;
    cfg->wakenet_init = false;
    if (enable_aec) {
        cfg->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
        cfg->vad_init = false;
    }
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    afe_config_check(cfg);
    ESP_LOGI(TAG,
             "probe config — type=%d mode=%d aec=%d se=%d ns=%d vad=%d agc=%d",
             (int)cfg->afe_type,
             (int)cfg->afe_mode,
             (int)cfg->aec_init,
             (int)cfg->se_init,
             (int)cfg->ns_init,
             (int)cfg->vad_init,
             (int)cfg->agc_init);

    handle = esp_afe_handle_from_config(cfg);
    if (handle == NULL) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config falhou");
        err = ESP_FAIL;
        goto done;
    }

    data = handle->create_from_config(cfg);
    if (data == NULL) {
        ESP_LOGE(TAG, "create_from_config falhou — PSRAM insuficiente?");
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    *handle_out = handle;
    *data_out = data;
    handle = NULL;
    data = NULL;
    err = ESP_OK;

done:
    if (handle != NULL && data != NULL) {
        handle->destroy(data);
        data = NULL;
    }
    if (cfg != NULL) {
        afe_config_free(cfg);
    }
    if (models != NULL) {
        esp_srmodel_deinit(models);
    }
    return err;
}

static esp_err_t create_vc_afe(const esp_afe_sr_iface_t **handle_out,
                               esp_afe_sr_data_t **data_out)
{
    return create_afe_instance("M", AFE_TYPE_VC, AFE_MODE_HIGH_PERF, false,
                               handle_out, data_out);
}

static esp_err_t create_aec_probe_afe(const esp_afe_sr_iface_t **handle_out,
                                      esp_afe_sr_data_t **data_out)
{
    return create_afe_instance("MR", AFE_TYPE_VC, AFE_MODE_HIGH_PERF, true,
                               handle_out, data_out);
}

static void update_io_status_locked(void)
{
    if (s.handle == NULL || s.data == NULL) {
        return;
    }
    s.status.feed_chunksize = s.handle->get_feed_chunksize(s.data);
    s.status.fetch_chunksize = s.handle->get_fetch_chunksize
                              ? s.handle->get_fetch_chunksize(s.data)
                              : 0;
    s.status.feed_channels = s.handle->get_feed_channel_num
                            ? s.handle->get_feed_channel_num(s.data)
                            : 0;
    s.status.fetch_channels = s.handle->get_fetch_channel_num
                             ? s.handle->get_fetch_channel_num(s.data)
                             : 0;
    s.status.sample_rate_hz = s.handle->get_samp_rate
                             ? s.handle->get_samp_rate(s.data)
                             : 0;
}

static void update_output_level_locked(const int16_t *data, int samples)
{
    if (data == NULL || samples <= 0) {
        return;
    }

    uint64_t sum_sq = 0;
    uint16_t peak = 0;
    for (int i = 0; i < samples; i++) {
        int32_t value = data[i];
        uint16_t abs_value = (uint16_t)((value < 0) ? -value : value);
        if (abs_value > peak) {
            peak = abs_value;
        }
        sum_sq += (uint64_t)((int64_t)value * (int64_t)value);
    }
    s.status.shadow_output_rms = (uint32_t)sqrtf((float)(sum_sq / (uint64_t)samples));
    s.status.shadow_output_peak = peak;
}

static void processed_ring_reset_locked(void)
{
    s.processed_ring_read = 0;
    s.processed_ring_write = 0;
    s.processed_ring_count = 0;
    s.status.processed_buffer_level = 0;
}

static esp_err_t processed_ring_ensure_locked(void)
{
    if (s.processed_ring != NULL) {
        return ESP_OK;
    }

    s.processed_ring = (int16_t *)heap_caps_malloc(
        PROCESSED_RING_SAMPLES * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (s.processed_ring == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s.processed_ring_capacity = PROCESSED_RING_SAMPLES;
    processed_ring_reset_locked();
    return ESP_OK;
}

static void processed_ring_free_locked(void)
{
    if (s.processed_ring != NULL) {
        heap_caps_free(s.processed_ring);
        s.processed_ring = NULL;
    }
    s.processed_ring_capacity = 0;
    processed_ring_reset_locked();
}

static void processed_ring_write_locked(const int16_t *data, uint16_t n)
{
    if (data == NULL || n == 0U || s.processed_ring == NULL ||
        s.processed_ring_capacity == 0U) {
        return;
    }

    for (uint16_t i = 0; i < n; i++) {
        if (s.processed_ring_count >= s.processed_ring_capacity) {
            s.processed_ring_read = (uint16_t)((s.processed_ring_read + 1U) %
                                               s.processed_ring_capacity);
            s.processed_ring_count--;
            s.status.processed_output_overruns++;
        }
        s.processed_ring[s.processed_ring_write] = data[i];
        s.processed_ring_write = (uint16_t)((s.processed_ring_write + 1U) %
                                            s.processed_ring_capacity);
        s.processed_ring_count++;
    }
    s.status.processed_buffer_level = s.processed_ring_count;
}

static void processed_ring_read_locked(int16_t *out, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++) {
        out[i] = s.processed_ring[s.processed_ring_read];
        s.processed_ring_read = (uint16_t)((s.processed_ring_read + 1U) %
                                           s.processed_ring_capacity);
        s.processed_ring_count--;
    }
    s.status.processed_buffer_level = s.processed_ring_count;
}

esp_err_t audio_processor_service_probe_once(void)
{
    const esp_afe_sr_iface_t *handle = NULL;
    esp_afe_sr_data_t *data = NULL;

    if (s.mutex) {
        xSemaphoreTake(s.mutex, portMAX_DELAY);
        if (s.status.shadow_active) {
            xSemaphoreGive(s.mutex);
            return ESP_ERR_INVALID_STATE;
        }
        s.status.probe_ran = true;
        s.status.probe_ok = false;
        s.status.last_error = ESP_FAIL;
        s.status.psram_before_kb = psram_free_kb();
        update_heap_status_locked();
        s.status.psram_after_create_kb = 0;
        s.status.psram_after_destroy_kb = 0;
        s.status.feed_chunksize = 0;
        s.status.fetch_chunksize = 0;
        s.status.feed_channels = 0;
        s.status.fetch_channels = 0;
        s.status.sample_rate_hz = 0;
        xSemaphoreGive(s.mutex);
    }

    ESP_LOGI(TAG, "probe AFE VC iniciado — PSRAM antes=%lu KB",
             (unsigned long)s.status.psram_before_kb);

    esp_err_t err = create_vc_afe(&handle, &data);
    if (err == ESP_OK) {
        xSemaphoreTake(s.mutex, portMAX_DELAY);
        s.handle = handle;
        s.data = data;
        s.status.psram_after_create_kb = psram_free_kb();
        update_io_status_locked();
        s.status.probe_ok = true;
        ESP_LOGI(TAG,
                 "probe AFE VC OK — PSRAM apos create=%lu KB feed=%d fetch=%d feed_ch=%d fetch_ch=%d rate=%d",
                 (unsigned long)s.status.psram_after_create_kb,
                 s.status.feed_chunksize,
                 s.status.fetch_chunksize,
                 s.status.feed_channels,
                 s.status.fetch_channels,
                 s.status.sample_rate_hz);
        s.handle = NULL;
        s.data = NULL;
        xSemaphoreGive(s.mutex);
    }

    if (handle != NULL && data != NULL) {
        handle->destroy(data);
    }

    xSemaphoreTake(s.mutex, portMAX_DELAY);
    s.status.psram_after_destroy_kb = psram_free_kb();
    s.status.last_error = err;
    ESP_LOGI(TAG, "probe AFE VC finalizado — err=%s PSRAM apos destroy=%lu KB",
             esp_err_to_name(err),
             (unsigned long)s.status.psram_after_destroy_kb);
    xSemaphoreGive(s.mutex);
    return err;
}

esp_err_t audio_processor_service_aec_probe_once(void)
{
    const esp_afe_sr_iface_t *handle = NULL;
    esp_afe_sr_data_t *data = NULL;

    if (s.mutex) {
        xSemaphoreTake(s.mutex, portMAX_DELAY);
        if (s.status.shadow_active) {
            xSemaphoreGive(s.mutex);
            return ESP_ERR_INVALID_STATE;
        }
        s.status.aec_probe_ran = true;
        s.status.aec_probe_ok = false;
        s.status.aec_last_error = ESP_FAIL;
        s.status.aec_psram_before_kb = psram_free_kb();
        s.status.aec_psram_after_create_kb = 0;
        s.status.aec_psram_after_destroy_kb = 0;
        update_board_caps_locked();
        update_heap_status_locked();
        xSemaphoreGive(s.mutex);
    }

    if (!nb_board_caps_get()->supports_device_aec) {
        ESP_LOGW(TAG,
                 "AEC probe bloqueado: placa sem canal limpo de referencia do speaker");
        xSemaphoreTake(s.mutex, portMAX_DELAY);
        s.status.aec_last_error = ESP_ERR_NOT_SUPPORTED;
        xSemaphoreGive(s.mutex);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!afe_runtime_heap_ok()) {
        ESP_LOGW(TAG,
                 "AEC probe bloqueado por margem de heap — internal=%lu KB dma_largest=%lu KB",
                 (unsigned long)internal_free_kb(),
                 (unsigned long)dma_largest_kb());
        xSemaphoreTake(s.mutex, portMAX_DELAY);
        s.status.aec_last_error = ESP_ERR_NO_MEM;
        xSemaphoreGive(s.mutex);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "probe AEC iniciado — PSRAM antes=%lu KB internal=%lu KB dma_largest=%lu KB",
             (unsigned long)s.status.aec_psram_before_kb,
             (unsigned long)internal_free_kb(),
             (unsigned long)dma_largest_kb());

    esp_err_t err = create_aec_probe_afe(&handle, &data);
    if (err == ESP_OK) {
        xSemaphoreTake(s.mutex, portMAX_DELAY);
        s.handle = handle;
        s.data = data;
        s.status.aec_psram_after_create_kb = psram_free_kb();
        update_io_status_locked();
        update_heap_status_locked();
        s.status.aec_probe_ok = true;
        ESP_LOGI(TAG,
                 "probe AEC OK — PSRAM apos create=%lu KB feed=%d fetch=%d feed_ch=%d fetch_ch=%d",
                 (unsigned long)s.status.aec_psram_after_create_kb,
                 s.status.feed_chunksize,
                 s.status.fetch_chunksize,
                 s.status.feed_channels,
                 s.status.fetch_channels);
        s.handle = NULL;
        s.data = NULL;
        xSemaphoreGive(s.mutex);
    }

    if (handle != NULL && data != NULL) {
        handle->destroy(data);
    }

    xSemaphoreTake(s.mutex, portMAX_DELAY);
    s.status.aec_psram_after_destroy_kb = psram_free_kb();
    update_heap_status_locked();
    s.status.aec_last_error = err;
    ESP_LOGI(TAG, "probe AEC finalizado — err=%s PSRAM apos destroy=%lu KB",
             esp_err_to_name(err),
             (unsigned long)s.status.aec_psram_after_destroy_kb);
    xSemaphoreGive(s.mutex);
    return err;
}

static void shadow_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "shadow task iniciada");

    while (1) {
        xSemaphoreTake(s.mutex, portMAX_DELAY);
        bool stop = s.status.shadow_stop_requested;
        const esp_afe_sr_iface_t *handle = s.handle;
        esp_afe_sr_data_t *data = s.data;
        xSemaphoreGive(s.mutex);

        if (stop || handle == NULL || data == NULL) {
            break;
        }

        afe_fetch_result_t *result = handle->fetch_with_delay
                                   ? handle->fetch_with_delay(data, pdMS_TO_TICKS(100))
                                   : handle->fetch(data);

        xSemaphoreTake(s.mutex, portMAX_DELAY);
        if (result == NULL) {
            s.status.shadow_fetch_nulls++;
        } else {
            s.status.shadow_fetch_chunks++;
            int result_samples = result->data_size / (int)sizeof(int16_t);
            update_output_level_locked(result->data, result_samples);
            if (s.status.processed_bridge_enabled &&
                s.status.processed_capture_active &&
                result_samples > 0 &&
                result_samples <= UINT16_MAX) {
                processed_ring_write_locked(result->data, (uint16_t)result_samples);
            }
            if ((s.status.shadow_fetch_chunks % SHADOW_LOG_FETCH_CHUNKS) == 1U) {
                ESP_LOGI(TAG,
                         "shadow AFE fetch=%lu nulls=%lu rms=%lu peak=%u psram=%lu KB",
                         (unsigned long)s.status.shadow_fetch_chunks,
                         (unsigned long)s.status.shadow_fetch_nulls,
                         (unsigned long)s.status.shadow_output_rms,
                         (unsigned)s.status.shadow_output_peak,
                         (unsigned long)psram_free_kb());
            }
        }
        s.status.shadow_psram_current_kb = psram_free_kb();
        xSemaphoreGive(s.mutex);
    }

    ESP_LOGI(TAG, "shadow task encerrada");
    s.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t audio_processor_service_shadow_start(void)
{
    if (!s.status.initialized || s.mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s.mutex, portMAX_DELAY);
    if (s.status.shadow_active) {
        xSemaphoreGive(s.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s.status.shadow_psram_start_kb = psram_free_kb();
    s.status.shadow_psram_current_kb = s.status.shadow_psram_start_kb;
    update_heap_status_locked();
    s.status.shadow_feed_chunks = 0;
    s.status.shadow_fetch_chunks = 0;
    s.status.shadow_fetch_nulls = 0;
    s.status.shadow_feed_drops = 0;
    s.status.shadow_output_rms = 0;
    s.status.shadow_output_peak = 0;
    s.status.processed_bridge_chunks = 0;
    s.status.processed_bridge_fallbacks = 0;
    s.status.processed_output_overruns = 0;
    s.status.processed_buffer_level = 0;
    s.status.processed_capture_active = false;
    s.status.shadow_stop_requested = false;
    s.feed_pos = 0;
    xSemaphoreGive(s.mutex);

    if (!afe_runtime_heap_ok()) {
        xSemaphoreTake(s.mutex, portMAX_DELAY);
        s.status.last_error = ESP_ERR_NO_MEM;
        update_heap_status_locked();
        xSemaphoreGive(s.mutex);
        ESP_LOGW(TAG,
                 "shadow AFE bloqueado por margem de heap — internal=%lu KB dma_largest=%lu KB",
                 (unsigned long)internal_free_kb(),
                 (unsigned long)dma_largest_kb());
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s.mutex, portMAX_DELAY);
    esp_err_t ring_err = processed_ring_ensure_locked();
    xSemaphoreGive(s.mutex);

    if (ring_err != ESP_OK) {
        xSemaphoreTake(s.mutex, portMAX_DELAY);
        s.status.last_error = ring_err;
        xSemaphoreGive(s.mutex);
        return ring_err;
    }

    const esp_afe_sr_iface_t *handle = NULL;
    esp_afe_sr_data_t *data = NULL;
    esp_err_t err = create_vc_afe(&handle, &data);
    if (err != ESP_OK) {
        xSemaphoreTake(s.mutex, portMAX_DELAY);
        s.status.last_error = err;
        xSemaphoreGive(s.mutex);
        return err;
    }

    xSemaphoreTake(s.mutex, portMAX_DELAY);
    s.handle = handle;
    s.data = data;
    s.status.shadow_active = true;
    s.status.shadow_psram_current_kb = psram_free_kb();
    update_heap_status_locked();
    update_io_status_locked();
    s.status.last_error = ESP_OK;
    ESP_LOGI(TAG,
             "shadow AFE iniciado — psram_start=%lu KB psram_now=%lu KB feed=%d fetch=%d",
             (unsigned long)s.status.shadow_psram_start_kb,
             (unsigned long)s.status.shadow_psram_current_kb,
             s.status.feed_chunksize,
             s.status.fetch_chunksize);
    xSemaphoreGive(s.mutex);

    BaseType_t rc = xTaskCreatePinnedToCore(
        shadow_task,
        "nb_afe_shadow",
        SHADOW_TASK_STACK,
        NULL,
        SHADOW_TASK_PRIORITY,
        &s.task,
        SHADOW_TASK_CORE
    );
    if (rc != pdPASS) {
        (void)audio_processor_service_shadow_stop();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t audio_processor_service_shadow_stop(void)
{
    if (s.mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s.mutex, portMAX_DELAY);
    if (!s.status.shadow_active) {
        xSemaphoreGive(s.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s.status.shadow_stop_requested = true;
    xSemaphoreGive(s.mutex);

    for (uint8_t i = 0; i < 20U && s.task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    xSemaphoreTake(s.mutex, portMAX_DELAY);
    const esp_afe_sr_iface_t *handle = s.handle;
    esp_afe_sr_data_t *data = s.data;
    s.handle = NULL;
    s.data = NULL;
    s.task = NULL;
    s.feed_pos = 0;
    s.status.processed_bridge_enabled = false;
    s.status.processed_capture_active = false;
    s.status.shadow_active = false;
    s.status.shadow_stop_requested = false;
    s.status.shadow_psram_current_kb = psram_free_kb();
    processed_ring_free_locked();
    xSemaphoreGive(s.mutex);

    if (handle != NULL && data != NULL) {
        handle->destroy(data);
    }

    xSemaphoreTake(s.mutex, portMAX_DELAY);
    s.status.shadow_psram_current_kb = psram_free_kb();
    update_heap_status_locked();
    ESP_LOGI(TAG,
             "shadow AFE parado — feed=%lu fetch=%lu nulls=%lu drops=%lu psram=%lu KB",
             (unsigned long)s.status.shadow_feed_chunks,
             (unsigned long)s.status.shadow_fetch_chunks,
             (unsigned long)s.status.shadow_fetch_nulls,
             (unsigned long)s.status.shadow_feed_drops,
             (unsigned long)s.status.shadow_psram_current_kb);
    xSemaphoreGive(s.mutex);
    return ESP_OK;
}

esp_err_t audio_processor_service_bridge_start(void)
{
    if (!s.status.initialized || s.mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    bool need_shadow = false;
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    need_shadow = !s.status.shadow_active;
    xSemaphoreGive(s.mutex);

    if (need_shadow) {
        esp_err_t err = audio_processor_service_shadow_start();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
    }

    xSemaphoreTake(s.mutex, portMAX_DELAY);
    if (!s.status.shadow_active || s.processed_ring == NULL) {
        s.status.last_error = ESP_ERR_INVALID_STATE;
        xSemaphoreGive(s.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    processed_ring_reset_locked();
    s.status.processed_bridge_enabled = true;
    s.status.processed_capture_active = false;
    s.status.processed_bridge_chunks = 0;
    s.status.processed_bridge_fallbacks = 0;
    s.status.processed_output_overruns = 0;
    s.status.last_error = ESP_OK;
    xSemaphoreGive(s.mutex);
    ESP_LOGI(TAG, "fonte processada do bridge habilitada");
    return ESP_OK;
}

esp_err_t audio_processor_service_bridge_stop(void)
{
    if (s.mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s.mutex, portMAX_DELAY);
    s.status.processed_bridge_enabled = false;
    s.status.processed_capture_active = false;
    processed_ring_reset_locked();
    xSemaphoreGive(s.mutex);
    ESP_LOGI(TAG, "fonte processada do bridge desabilitada");
    return ESP_OK;
}

void audio_processor_service_bridge_capture_begin(void)
{
    if (s.mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s.mutex, 0) != pdTRUE) {
        return;
    }
    if (s.status.processed_bridge_enabled && s.status.shadow_active) {
        processed_ring_reset_locked();
        s.status.processed_capture_active = true;
    }
    xSemaphoreGive(s.mutex);
}

void audio_processor_service_bridge_capture_end(void)
{
    if (s.mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s.mutex, 0) != pdTRUE) {
        return;
    }
    s.status.processed_capture_active = false;
    processed_ring_reset_locked();
    xSemaphoreGive(s.mutex);
}

bool audio_processor_service_read_bridge_processed(int16_t *out, uint16_t n)
{
    if (out == NULL || n == 0U || s.mutex == NULL) {
        return false;
    }
    if (!s.status.processed_bridge_enabled || !s.status.processed_capture_active) {
        return false;
    }
    if (xSemaphoreTake(s.mutex, 0) != pdTRUE) {
        return false;
    }

    bool ok = false;
    if (s.status.processed_bridge_enabled &&
        s.status.processed_capture_active &&
        s.processed_ring != NULL &&
        s.processed_ring_count >= n) {
        processed_ring_read_locked(out, n);
        s.status.processed_bridge_chunks++;
        ok = true;
    } else if (s.status.processed_bridge_enabled && s.status.processed_capture_active) {
        s.status.processed_bridge_fallbacks++;
    }
    xSemaphoreGive(s.mutex);
    return ok;
}

void audio_processor_service_feed_shadow(const int16_t *pcm, uint16_t n)
{
    if (pcm == NULL || n == 0U || s.mutex == NULL) {
        return;
    }
    if (!s.status.shadow_active || s.status.shadow_stop_requested) {
        return;
    }
    if (xSemaphoreTake(s.mutex, 0) != pdTRUE) {
        return;
    }

    const esp_afe_sr_iface_t *handle = s.handle;
    esp_afe_sr_data_t *data = s.data;
    int feed_chunksize = s.status.feed_chunksize;
    if (!s.status.shadow_active || handle == NULL || data == NULL ||
        feed_chunksize <= 0 || feed_chunksize > (int)FEED_BUF_MAX) {
        xSemaphoreGive(s.mutex);
        return;
    }

    uint16_t offset = 0;
    while (offset < n) {
        uint16_t space = (uint16_t)(feed_chunksize - (int)s.feed_pos);
        uint16_t to_copy = ((uint16_t)(n - offset) < space)
                         ? (uint16_t)(n - offset)
                         : space;
        memcpy(&s.feed_buf[s.feed_pos], &pcm[offset], to_copy * sizeof(int16_t));
        s.feed_pos += to_copy;
        offset += to_copy;

        if (s.feed_pos >= (uint16_t)feed_chunksize) {
            handle->feed(data, s.feed_buf);
            s.feed_pos = 0;
            s.status.shadow_feed_chunks++;
        }
    }
    xSemaphoreGive(s.mutex);
}

esp_err_t audio_processor_service_init(void)
{
    if (s.status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s, 0, sizeof(s));
    s.mutex = xSemaphoreCreateMutexStatic(&s.mutex_buf);
    if (s.mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s.status.initialized = true;
    s.status.last_error = ESP_OK;
    s.status.aec_last_error = ESP_OK;
    update_board_caps_locked();
    update_heap_status_locked();
    s.status.enabled = probe_enabled_from_nvs();

    if (!s.status.enabled) {
        ESP_LOGI(TAG, "AFE VC probe desabilitado (NVS %s/%s=0)",
                 NVS_NS_SERVICE, NVS_KEY_VOICE_AFE_PROBE);
        return ESP_OK;
    }

    esp_err_t err = audio_processor_service_probe_once();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AFE VC probe falhou: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

void audio_processor_service_get_status(nb_audio_processor_status_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s.mutex) {
        xSemaphoreTake(s.mutex, portMAX_DELAY);
        s.status.shadow_psram_current_kb = psram_free_kb();
        update_board_caps_locked();
        update_heap_status_locked();
        *out = s.status;
        xSemaphoreGive(s.mutex);
    } else {
        *out = s.status;
    }
}
