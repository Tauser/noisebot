/*
 * vision_service.c - Percepcao visual leve sobre camera_service.
 */

#include "vision_service.h"

#include "camera_service.h"
#include "event_bus.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

#define TAG "nb_vision"

static bool s_initialized = false;
static SemaphoreHandle_t s_mutex = NULL;
static nb_vision_observation_t s_last = {0};
static nb_vision_presence_status_t s_presence = {
    .state = NB_VISION_PRESENCE_ABSENT,
};

#define NB_VISION_PRESENCE_DETECTED_MS 300U
#define NB_VISION_PRESENCE_LOST_MS 120000U
#define NB_VISION_PRESENCE_SCORE_DETECTED 55U

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

const char *vision_service_presence_state_name(nb_vision_presence_state_t state)
{
    switch (state) {
    case NB_VISION_PRESENCE_ABSENT: return "absent";
    case NB_VISION_PRESENCE_CANDIDATE: return "candidate";
    case NB_VISION_PRESENCE_PRESENT: return "present";
    case NB_VISION_PRESENCE_UNKNOWN:
    default: return "unknown";
    }
}

static uint8_t clamp_u8(uint16_t value)
{
    return (value > 100U) ? 100U : (uint8_t)value;
}

static uint8_t presence_score(const nb_vision_observation_t *obs)
{
    if (!obs || !obs->valid) {
        return 0U;
    }
    if (obs->scene == NB_VISION_SCENE_DARK ||
        obs->scene == NB_VISION_SCENE_FLAT ||
        obs->scene == NB_VISION_SCENE_UNKNOWN) {
        return clamp_u8((uint16_t)obs->motion_score);
    }

    uint16_t score = 0U;
    score += (uint16_t)((obs->motion_score > 30U) ? 60U : obs->motion_score * 2U);
    if (obs->contrast >= 24U) {
        score += (uint16_t)((obs->contrast > 84U) ? 25U : ((obs->contrast - 24U) / 3U));
    }
    if (obs->luma_avg >= 45U && obs->luma_avg <= 210U) {
        score += 15U;
    }
    return clamp_u8(score);
}

static void publish_presence_event(nb_event_type_t type, uint8_t score, uint32_t ts_ms)
{
    nb_event_t evt = {
        .type = type,
        .timestamp_ms = ts_ms,
        .data.u32 = score,
    };
    esp_err_t err = nb_event_publish_async(&evt);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "falha ao publicar evento de presença: %s", esp_err_to_name(err));
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
    memset(&s_presence, 0, sizeof(s_presence));
    s_presence.state = NB_VISION_PRESENCE_ABSENT;
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
    (void)vision_service_evaluate_presence(&obs, NULL);
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

esp_err_t vision_service_evaluate_presence(const nb_vision_observation_t *obs,
                                           nb_vision_presence_status_t *out)
{
    if (!obs) {
        return ESP_ERR_INVALID_ARG;
    }

    nb_vision_presence_status_t status = {0};
    bool publish_detected = false;
    bool publish_lost = false;
    uint8_t score = presence_score(obs);
    uint32_t now_ms = obs->timestamp_ms;
    if (now_ms == 0U) {
        now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    }
    bool candidate = score >= NB_VISION_PRESENCE_SCORE_DETECTED;

    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        status = s_presence;
        status.score = score;

        if (candidate) {
            if (status.candidate_since_ms == 0U) {
                status.candidate_since_ms = now_ms;
            }
            status.absent_since_ms = 0U;
            status.absent_samples = 0U;
            status.stable_samples++;
            if (status.state != NB_VISION_PRESENCE_PRESENT &&
                (now_ms - status.candidate_since_ms) >= NB_VISION_PRESENCE_DETECTED_MS) {
                status.state = NB_VISION_PRESENCE_PRESENT;
                status.last_transition_ms = now_ms;
                status.transition_count++;
                publish_detected = true;
            } else if (status.state != NB_VISION_PRESENCE_PRESENT) {
                status.state = NB_VISION_PRESENCE_CANDIDATE;
            }
        } else {
            if (status.absent_since_ms == 0U) {
                status.absent_since_ms = now_ms;
            }
            status.candidate_since_ms = 0U;
            status.stable_samples = 0U;
            status.absent_samples++;
            if (status.state == NB_VISION_PRESENCE_PRESENT &&
                (now_ms - status.absent_since_ms) >= NB_VISION_PRESENCE_LOST_MS) {
                status.state = NB_VISION_PRESENCE_ABSENT;
                status.last_transition_ms = now_ms;
                status.transition_count++;
                publish_lost = true;
            } else if (status.state != NB_VISION_PRESENCE_PRESENT) {
                status.state = NB_VISION_PRESENCE_ABSENT;
            }
        }

        s_presence = status;
        xSemaphoreGive(s_mutex);
    } else {
        return ESP_ERR_TIMEOUT;
    }

    if (publish_detected) {
        publish_presence_event(NB_EVT_PRESENCE_DETECTED, score, now_ms);
    } else if (publish_lost) {
        publish_presence_event(NB_EVT_PRESENCE_LOST, score, now_ms);
    }

    if (out) {
        *out = status;
    }
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

void vision_service_get_presence(nb_vision_presence_status_t *out)
{
    if (!out) {
        return;
    }
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        *out = s_presence;
        xSemaphoreGive(s_mutex);
    } else {
        memset(out, 0, sizeof(*out));
        out->state = NB_VISION_PRESENCE_UNKNOWN;
    }
}
