/*
 * wake_service.c — Wake Word via ESP-SR WakeNet (Layer 4)
 */

#include "wake_service.h"
#include "nb_events.h"
#include "event_bus.h"

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "model_path.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_heap_caps.h"

#include <string.h>

#define TAG "wake_svc"

#define WAKE_TASK_STACK    4096U
#define WAKE_TASK_PRIORITY 7U
#define WAKE_TASK_CORE     0

/* Buffer de acumulação: AFE tipicamente requer 512 samples @ 16kHz */
#define FEED_BUF_MAX  512U
#define WAKE_REARM_GUARD_MS  350U
#define WAKE_GAIN_LOG_INTERVAL_MS  2000U

/* Ganho de entrada aplicado antes do feed: s_sa_buf chega em ~±400 int16
 * (speech típico), WakeNet espera ~±4000-8000. Fator 16 = +24 dB compensa
 * o duplo shift >>8 do pipeline mic (24-bit→int32 no HAL, int32→int16 no svc). */
#define WAKE_INPUT_GAIN  16

static struct {
    bool                         initialized;
    bool                         enabled;
    const esp_afe_sr_iface_t    *handle;
    esp_afe_sr_data_t           *data;
    int                          feed_chunksize;
    int16_t                      feed_buf[FEED_BUF_MAX];
    uint16_t                     feed_pos;
    volatile bool                suspended;
    volatile bool                armed;
    volatile bool                detection_latched;
    volatile uint32_t            guard_until_ms;
    uint32_t                     gain_log_next_ms;
    uint32_t                     gain_log_samples;
    uint64_t                     gain_log_sum_sq;
    uint16_t                     gain_log_raw_peak;
    uint16_t                     gain_log_post_peak;
    uint32_t                     gain_log_saturated;
    SemaphoreHandle_t            mutex;
    StaticSemaphore_t            mutex_buf;
} s;

static uint16_t abs_i16(int16_t v)
{
    return (v == INT16_MIN) ? 32768U : (uint16_t)((v < 0) ? -v : v);
}

static uint32_t isqrt_u32(uint32_t x)
{
    uint32_t result = 0;
    uint32_t bit = 1UL << 30;
    while (bit > x) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (x >= result + bit) {
            x -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

static void wake_service_set_suspended(bool suspended, bool latch_detection)
{
    if (!s.enabled || !s.data || !s.mutex) return;

    xSemaphoreTake(s.mutex, portMAX_DELAY);
    s.suspended = suspended;
    s.feed_pos = 0;
    if (s.handle->reset_buffer) {
        s.handle->reset_buffer(s.data);
    }
    if (suspended) {
        s.armed = false;
        if (latch_detection) {
            s.detection_latched = true;
        }
        if (s.handle->disable_wakenet) {
            s.handle->disable_wakenet(s.data);
        }
    } else {
        s.armed = true;
        s.detection_latched = false;
        s.guard_until_ms = (uint32_t)(esp_timer_get_time() / 1000LL)
                         + WAKE_REARM_GUARD_MS;
        s.gain_log_next_ms = s.guard_until_ms + WAKE_GAIN_LOG_INTERVAL_MS;
        s.gain_log_samples = 0;
        s.gain_log_sum_sq = 0;
        s.gain_log_raw_peak = 0;
        s.gain_log_post_peak = 0;
        s.gain_log_saturated = 0;
        if (s.handle->enable_wakenet) {
            s.handle->enable_wakenet(s.data);
        }
    }
    xSemaphoreGive(s.mutex);
}

static void wake_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "wake_task iniciada");
    while (1) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
        if (s.suspended || (int32_t)(s.guard_until_ms - now_ms) > 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        afe_fetch_result_t *res = s.handle->fetch_with_delay
                                ? s.handle->fetch_with_delay(s.data, portMAX_DELAY)
                                : s.handle->fetch(s.data);
        if (res == NULL) continue;
        if (res->wakeup_state == WAKENET_DETECTED) {
            if (s.suspended || !s.armed || s.detection_latched) {
                continue;
            }
            ESP_LOGI(TAG, "wake word detectada — channel=%d",
                     res->trigger_channel_id);
            wake_service_set_suspended(true, true);
            nb_event_t evt = {
                .type         = NB_EVT_WAKE_WORD_DETECTED,
                .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000LL),
                .data.u32     = 0,
            };
            nb_event_publish_async(&evt);
        }
    }
}

esp_err_t wake_service_init(void)
{
    if (s.initialized) return ESP_ERR_INVALID_STATE;

    /* NVS: ww_enabled (padrão = habilitado se chave ausente) */
    s.enabled = true;
    nvs_handle_t nvs;
    if (nvs_open("nb_svc", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t val;
        if (nvs_get_u8(nvs, "ww_enabled", &val) == ESP_OK) {
            s.enabled = (val != 0);
        }
        nvs_close(nvs);
    }

    if (!s.enabled) {
        ESP_LOGI(TAG, "wake word desabilitado via NVS");
        s.initialized = true;
        return ESP_OK;
    }

    ESP_LOGI(TAG, "PSRAM livre pré-AFE: %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U));

    /* Carrega modelos da partição "model" (preenchida pelo build system) */
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGE(TAG, "esp_srmodel_init falhou — partição 'model' não encontrada");
        return ESP_FAIL;
    }

    /* Config AFE: microfone mono ("M"), tipo SR, modo low-cost */
    afe_config_t *cfg = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!cfg) {
        ESP_LOGE(TAG, "afe_config_init falhou");
        esp_srmodel_deinit(models);
        return ESP_FAIL;
    }

    /* Sem AEC (sem loopback de speaker) e sem SE (microfone mono) */
    cfg->aec_init = false;
    cfg->se_init  = false;

    /* VAD desabilitado: com VAD ativo o pipeline é VAD→WakeNet e o VAD
     * gatea o WakeNet — se o WebRTC não detecta fala primeiro, WakeNet
     * nunca recebe o áudio. Sem VAD o WakeNet processa continuamente. */
    cfg->vad_init = false;

    /* afe_linear_gain é ganho de SAÍDA do AFE, não de entrada do WakeNet.
     * O ganho de entrada é aplicado em wake_service_feed() via WAKE_INPUT_GAIN. */
    cfg->afe_linear_gain = 1.0f;

    /* Alocar memória preferencialmente em PSRAM */
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    afe_config_check(cfg);

    s.handle = esp_afe_handle_from_config(cfg);
    if (!s.handle) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config falhou");
        afe_config_free(cfg);
        esp_srmodel_deinit(models);
        return ESP_FAIL;
    }

    s.data = s.handle->create_from_config(cfg);
    afe_config_free(cfg);
    esp_srmodel_deinit(models);

    if (!s.data) {
        ESP_LOGE(TAG, "AFE create_from_config falhou — PSRAM insuficiente?");
        return ESP_FAIL;
    }

    s.feed_chunksize = s.handle->get_feed_chunksize(s.data);
    if (s.feed_chunksize <= 0 || (uint32_t)s.feed_chunksize > FEED_BUF_MAX) {
        ESP_LOGE(TAG, "feed_chunksize=%d fora do range esperado", s.feed_chunksize);
        s.handle->destroy(s.data);
        s.data = NULL;
        return ESP_FAIL;
    }
    s.feed_pos = 0;
    s.suspended = false;
    s.armed = true;
    s.detection_latched = false;
    s.guard_until_ms = 0;
    s.gain_log_next_ms = (uint32_t)(esp_timer_get_time() / 1000LL)
                       + WAKE_GAIN_LOG_INTERVAL_MS;
    s.mutex = xSemaphoreCreateMutexStatic(&s.mutex_buf);
    if (!s.mutex) {
        ESP_LOGE(TAG, "mutex wake_service falhou");
        s.handle->destroy(s.data);
        s.data = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "PSRAM livre pós-AFE:  %u KB (feed_chunksize=%d)",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
             s.feed_chunksize);

    BaseType_t rc = xTaskCreatePinnedToCore(
        wake_task, "nb_wake_task",
        WAKE_TASK_STACK, NULL,
        WAKE_TASK_PRIORITY, NULL,
        WAKE_TASK_CORE
    );
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore falhou");
        s.handle->destroy(s.data);
        s.data = NULL;
        return ESP_ERR_NO_MEM;
    }

    s.initialized = true;
    ESP_LOGI(TAG, "inicializado");
    return ESP_OK;
}

void wake_service_feed(const int16_t *pcm, uint16_t n)
{
    if (!s.initialized || !s.enabled || !s.data || n == 0) return;
    if (s.suspended || !s.mutex) return;

    const int16_t *src = pcm;
    uint16_t remaining = n;

    xSemaphoreTake(s.mutex, portMAX_DELAY);
    while (remaining > 0) {
        if (s.suspended) break;
        uint16_t space   = (uint16_t)(s.feed_chunksize - (int)s.feed_pos);
        uint16_t to_copy = (remaining < space) ? remaining : space;
        for (uint16_t i = 0; i < to_copy; i++) {
            int32_t sample = (int32_t)src[i];
            int32_t v = sample * WAKE_INPUT_GAIN;
            uint16_t raw_abs = abs_i16(src[i]);
            if (raw_abs > s.gain_log_raw_peak) {
                s.gain_log_raw_peak = raw_abs;
            }
            s.gain_log_sum_sq += (uint64_t)(sample * sample);
            s.gain_log_samples++;
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            if (v == 32767 || v == -32768) {
                s.gain_log_saturated++;
            }
            uint16_t post_abs = abs_i16((int16_t)v);
            if (post_abs > s.gain_log_post_peak) {
                s.gain_log_post_peak = post_abs;
            }
            s.feed_buf[s.feed_pos + i] = (int16_t)v;
        }
        s.feed_pos += to_copy;
        src        += to_copy;
        remaining  -= to_copy;
        if (s.feed_pos >= (uint16_t)s.feed_chunksize) {
            s.handle->feed(s.data, s.feed_buf);
            s.feed_pos = 0;
        }
    }
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    if ((int32_t)(now_ms - s.gain_log_next_ms) >= 0 && s.gain_log_samples > 0U) {
        uint32_t mean_sq = (uint32_t)(s.gain_log_sum_sq / s.gain_log_samples);
        uint32_t rms = isqrt_u32(mean_sq);
        ESP_LOGI(TAG,
                 "gain diag: raw_rms=%lu raw_peak=%u gain=%d post_peak=%u saturated=%lu/%lu",
                 (unsigned long)rms,
                 (unsigned)s.gain_log_raw_peak,
                 WAKE_INPUT_GAIN,
                 (unsigned)s.gain_log_post_peak,
                 (unsigned long)s.gain_log_saturated,
                 (unsigned long)s.gain_log_samples);
        s.gain_log_next_ms = now_ms + WAKE_GAIN_LOG_INTERVAL_MS;
        s.gain_log_samples = 0;
        s.gain_log_sum_sq = 0;
        s.gain_log_raw_peak = 0;
        s.gain_log_post_peak = 0;
        s.gain_log_saturated = 0;
    }
    xSemaphoreGive(s.mutex);
}

bool wake_service_is_active(void)
{
    return s.initialized && s.enabled && (s.data != NULL);
}

void wake_service_suspend(void)
{
    wake_service_set_suspended(true, false);
}

void wake_service_rearm(void)
{
    wake_service_set_suspended(false, false);
    if (wake_service_is_active()) {
        ESP_LOGI(TAG, "WakeNet rearmado");
    }
}
