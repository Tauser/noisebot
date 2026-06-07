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
#include "freertos/task.h"
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
#define NB_VISION_PRESENCE_SCORE_STRONG 60U
#define NB_VISION_PRESENCE_RETAIN_MS 2500U
#define NB_VISION_PRESENCE_MIN_SAMPLES 2U
#define NB_VISION_POLL_DEFAULT_INTERVAL_MS 300U
#define NB_VISION_POLL_MIN_INTERVAL_MS 250U
#define NB_VISION_POLL_MAX_INTERVAL_MS 10000U
#define NB_VISION_POLL_TASK_STACK 4096U
#define NB_VISION_POLL_TASK_PRIORITY 4U

static TaskHandle_t s_poll_task = NULL;
static nb_vision_poll_status_t s_poll = {0};
static uint64_t s_poll_capture_sum_ms = 0U;
static nb_vision_scene_t s_last_logged_scene = NB_VISION_SCENE_UNKNOWN;
static bool s_last_logged_scene_valid = false;

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

static esp_err_t publish_presence_event(nb_event_type_t type, uint8_t score, uint32_t ts_ms)
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
    return err;
}

static void record_presence_event_publish(nb_event_type_t type, uint32_t ts_ms)
{
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (type == NB_EVT_PRESENCE_DETECTED) {
            s_presence.detected_event_count++;
        } else if (type == NB_EVT_PRESENCE_LOST) {
            s_presence.lost_event_count++;
        }
        s_presence.last_event_ms = ts_ms;
        xSemaphoreGive(s_mutex);
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
    if (camera_service_get_mode() != NB_CAMERA_MODE_SAFE_QQVGA) {
        esp_err_t mode_err = camera_service_set_mode(NB_CAMERA_MODE_SAFE_QQVGA);
        if (mode_err != ESP_OK) {
            return mode_err;
        }
    }

    int64_t start_us = esp_timer_get_time();
    nb_camera_observation_t cam_obs;
    esp_err_t err = camera_service_observe_scene(&cam_obs);
    if (err != ESP_OK) {
        return err;
    }

    nb_camera_scene_metrics_t metrics = cam_obs.scene;

    nb_vision_observation_t obs = {
        .valid = metrics.valid,
        .timestamp_ms = metrics.valid
                      ? metrics.timestamp_ms
                      : (uint32_t)(esp_timer_get_time() / 1000LL),
        .width = metrics.width,
        .height = metrics.height,
        .jpeg_bytes = 0U,
        .capture_ms = cam_obs.capture_ms,
        .luma_avg = metrics.luma_avg,
        .luma_min = metrics.luma_min,
        .luma_max = metrics.luma_max,
        .contrast = metrics.contrast,
        .motion_score = metrics.motion_score,
        .spatial_score = metrics.spatial_score,
        .scene = classify_scene(&metrics),
    };
    if (obs.capture_ms == 0U) {
        obs.capture_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000LL);
    }
    if (obs.capture_ms == 0U) {
        obs.capture_ms = 1U;
    }

    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
        s_last = obs;
        xSemaphoreGive(s_mutex);
    }
    (void)vision_service_evaluate_presence(&obs, NULL);
    *out = obs;

    if (!s_last_logged_scene_valid || obs.scene != s_last_logged_scene) {
        ESP_LOGI(TAG, "cena mudou: %s -> %s (luma=%u motion=%u)",
                 s_last_logged_scene_valid ? vision_service_scene_name(s_last_logged_scene) : "n/a",
                 vision_service_scene_name(obs.scene),
                 (unsigned)obs.luma_avg,
                 (unsigned)obs.motion_score);
        s_last_logged_scene = obs.scene;
        s_last_logged_scene_valid = true;
    }

    ESP_LOGD(TAG,
             "observacao scene=%s luma=%u contrast=%u motion=%u spatial=%u jpeg=%uB ms=%lu",
             vision_service_scene_name(obs.scene),
             (unsigned)obs.luma_avg,
             (unsigned)obs.contrast,
             (unsigned)obs.motion_score,
             (unsigned)obs.spatial_score,
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
    uint8_t event_score = score;
    uint32_t now_ms = obs->timestamp_ms;
    if (now_ms == 0U) {
        now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    }
    bool raw_candidate = score >= NB_VISION_PRESENCE_SCORE_DETECTED;

    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        status = s_presence;
        uint8_t previous_score = status.score;
        bool retained_candidate =
            !raw_candidate &&
            status.state == NB_VISION_PRESENCE_CANDIDATE &&
            status.candidate_since_ms != 0U &&
            previous_score >= NB_VISION_PRESENCE_SCORE_STRONG &&
            (now_ms - status.candidate_since_ms) <= NB_VISION_PRESENCE_RETAIN_MS;
        bool candidate = raw_candidate || retained_candidate;
        if (retained_candidate) {
            event_score = previous_score;
        }
        status.score = score;

        if (candidate) {
            if (status.candidate_since_ms == 0U) {
                status.candidate_since_ms = now_ms;
            }
            status.absent_since_ms = 0U;
            status.absent_samples = 0U;
            if (raw_candidate) {
                status.stable_samples++;
            }
            if (status.state != NB_VISION_PRESENCE_PRESENT &&
                raw_candidate &&
                status.stable_samples >= NB_VISION_PRESENCE_MIN_SAMPLES &&
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
        if (publish_presence_event(NB_EVT_PRESENCE_DETECTED, event_score, now_ms) == ESP_OK) {
            record_presence_event_publish(NB_EVT_PRESENCE_DETECTED, now_ms);
        }
    } else if (publish_lost) {
        if (publish_presence_event(NB_EVT_PRESENCE_LOST, score, now_ms) == ESP_OK) {
            record_presence_event_publish(NB_EVT_PRESENCE_LOST, now_ms);
        }
    }

    if (out) {
        vision_service_get_presence(out);
    }
    return ESP_OK;
}

static uint32_t clamp_poll_interval_ms(uint32_t interval_ms)
{
    if (interval_ms == 0U) {
        return NB_VISION_POLL_DEFAULT_INTERVAL_MS;
    }
    if (interval_ms < NB_VISION_POLL_MIN_INTERVAL_MS) {
        return NB_VISION_POLL_MIN_INTERVAL_MS;
    }
    if (interval_ms > NB_VISION_POLL_MAX_INTERVAL_MS) {
        return NB_VISION_POLL_MAX_INTERVAL_MS;
    }
    return interval_ms;
}

static void vision_poll_task(void *arg)
{
    (void)arg;

    while (true) {
        uint32_t interval_ms = NB_VISION_POLL_DEFAULT_INTERVAL_MS;
        bool stop_requested = false;
        if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            interval_ms = s_poll.interval_ms;
            stop_requested = s_poll.stop_requested;
            xSemaphoreGive(s_mutex);
        }
        if (stop_requested) {
            break;
        }

        nb_vision_observation_t obs;
        esp_err_t err = vision_service_observe(&obs);
        if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (err == ESP_OK) {
                s_poll.sample_count++;
                s_poll.last_sample_ms = obs.timestamp_ms;
                s_poll.last_capture_ms = obs.capture_ms;
                if (obs.capture_ms > s_poll.max_capture_ms) {
                    s_poll.max_capture_ms = obs.capture_ms;
                }
                s_poll_capture_sum_ms += obs.capture_ms;
                if (s_poll.sample_count > 0U) {
                    s_poll.avg_capture_ms =
                        (uint32_t)(s_poll_capture_sum_ms / s_poll.sample_count);
                }
                s_poll.last_error = ESP_OK;
            } else {
                s_poll.fail_count++;
                s_poll.last_error = err;
            }
            xSemaphoreGive(s_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }

    if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        s_poll.running = false;
        s_poll.stop_requested = false;
        s_poll_task = NULL;
        xSemaphoreGive(s_mutex);
    }
    vTaskDelete(NULL);
}

esp_err_t vision_service_poll_start(uint32_t interval_ms)
{
    if (!s_initialized) {
        esp_err_t init_err = vision_service_init();
        if (init_err != ESP_OK && init_err != ESP_ERR_INVALID_STATE) {
            return init_err;
        }
    }
    if (!s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    interval_ms = clamp_poll_interval_ms(interval_ms);
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_poll.running) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_poll, 0, sizeof(s_poll));
    s_poll.running = true;
    s_poll.interval_ms = interval_ms;
    s_poll.started_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    s_poll.last_error = ESP_OK;
    s_poll_capture_sum_ms = 0U;
    xSemaphoreGive(s_mutex);

    BaseType_t rc = xTaskCreate(vision_poll_task,
                                "nb_vision_poll",
                                NB_VISION_POLL_TASK_STACK,
                                NULL,
                                NB_VISION_POLL_TASK_PRIORITY,
                                &s_poll_task);
    if (rc != pdPASS) {
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            memset(&s_poll, 0, sizeof(s_poll));
            s_poll.last_error = ESP_ERR_NO_MEM;
            xSemaphoreGive(s_mutex);
        }
        s_poll_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "poll visual iniciado interval_ms=%lu", (unsigned long)interval_ms);
    return ESP_OK;
}

esp_err_t vision_service_poll_stop(void)
{
    if (!s_mutex) {
        return ESP_OK;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_poll.running) {
        s_poll.stop_requested = true;
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void vision_service_get_poll_status(nb_vision_poll_status_t *out)
{
    if (!out) {
        return;
    }
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        *out = s_poll;
        xSemaphoreGive(s_mutex);
    } else {
        memset(out, 0, sizeof(*out));
        out->last_error = ESP_OK;
    }
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

void vision_service_reset_presence(void)
{
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(&s_presence, 0, sizeof(s_presence));
        s_presence.state = NB_VISION_PRESENCE_ABSENT;
        xSemaphoreGive(s_mutex);
    }
}
