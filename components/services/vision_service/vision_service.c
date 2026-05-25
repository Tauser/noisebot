/*
 * vision_service.c - Percepcao visual leve sobre camera_service.
 */

#include "vision_service.h"

#include "camera_service.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

#define TAG "nb_vision"

static bool s_initialized = false;
static SemaphoreHandle_t s_mutex = NULL;
static nb_vision_observation_t s_last = {0};

static nb_vision_scene_t classify_scene(const nb_camera_scene_metrics_t *m)
{
    if (!m || !m->valid) {
        return NB_VISION_SCENE_UNKNOWN;
    }
    if (m->contrast < 18U) {
        return NB_VISION_SCENE_FLAT;
    }
    if (m->luma_avg < 45U) {
        return NB_VISION_SCENE_DARK;
    }
    if (m->luma_avg < 90U) {
        return NB_VISION_SCENE_DIM;
    }
    if (m->luma_avg > 210U) {
        return NB_VISION_SCENE_BRIGHT;
    }
    return NB_VISION_SCENE_NORMAL;
}

const char *vision_service_scene_name(nb_vision_scene_t scene)
{
    switch (scene) {
    case NB_VISION_SCENE_DARK: return "dark";
    case NB_VISION_SCENE_DIM: return "dim";
    case NB_VISION_SCENE_NORMAL: return "normal";
    case NB_VISION_SCENE_BRIGHT: return "bright";
    case NB_VISION_SCENE_FLAT: return "flat";
    case NB_VISION_SCENE_UNKNOWN:
    default: return "unknown";
    }
}

esp_err_t vision_service_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t cam_err = camera_service_init();
    if (cam_err != ESP_OK && cam_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "camera_service_init falhou: %s", esp_err_to_name(cam_err));
        return cam_err;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "falha ao criar mutex");
        return ESP_ERR_NO_MEM;
    }
    memset(&s_last, 0, sizeof(s_last));
    s_initialized = true;
    ESP_LOGI(TAG, "vision_service inicializado");
    return ESP_OK;
}

bool vision_service_is_available(void)
{
    return s_initialized && camera_service_is_available();
}

esp_err_t vision_service_observe(nb_vision_observation_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        esp_err_t init_err = vision_service_init();
        if (init_err != ESP_OK && init_err != ESP_ERR_INVALID_STATE) {
            return init_err;
        }
    }
    if (!camera_service_is_supported()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    nb_camera_snapshot_t snap;
    int64_t start_us = esp_timer_get_time();
    esp_err_t err = camera_service_capture_snapshot(&snap);
    if (err != ESP_OK) {
        return err;
    }

    nb_camera_scene_metrics_t metrics;
    camera_service_get_scene_metrics(&metrics);

    nb_vision_observation_t obs = {
        .valid = metrics.valid,
        .timestamp_ms = metrics.valid
                      ? metrics.timestamp_ms
                      : (uint32_t)(esp_timer_get_time() / 1000LL),
        .width = snap.width,
        .height = snap.height,
        .jpeg_bytes = snap.len,
        .capture_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000LL),
        .luma_avg = metrics.luma_avg,
        .luma_min = metrics.luma_min,
        .luma_max = metrics.luma_max,
        .contrast = metrics.contrast,
        .motion_score = metrics.motion_score,
        .scene = classify_scene(&metrics),
    };
    if (obs.capture_ms == 0U) {
        obs.capture_ms = 1U;
    }

    camera_service_release_snapshot();

    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
        s_last = obs;
        xSemaphoreGive(s_mutex);
    }
    *out = obs;

    ESP_LOGI(TAG,
             "observacao scene=%s luma=%u contrast=%u motion=%u jpeg=%uB ms=%lu",
             vision_service_scene_name(obs.scene),
             (unsigned)obs.luma_avg,
             (unsigned)obs.contrast,
             (unsigned)obs.motion_score,
             (unsigned)obs.jpeg_bytes,
             (unsigned long)obs.capture_ms);
    return ESP_OK;
}

void vision_service_get_last(nb_vision_observation_t *out)
{
    if (!out) {
        return;
    }
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        *out = s_last;
        xSemaphoreGive(s_mutex);
    } else {
        memset(out, 0, sizeof(*out));
    }
}
