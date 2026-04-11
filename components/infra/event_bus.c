#include "event_bus.h"
#include "logger.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "event_bus";

typedef struct {
    nb_event_type_t type;
    QueueHandle_t   queue;
    bool            active;
} nb_subscriber_t;

static nb_subscriber_t   s_subs[EVENT_BUS_MAX_SUBSCRIBERS];
static SemaphoreHandle_t s_mutex;
static bool              s_initialized = false;

/* ------------------------------------------------------------------ */

esp_err_t event_bus_init(void) {
    memset(s_subs, 0, sizeof(s_subs));
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    s_initialized = true;
    NB_LOGI(TAG, "Event bus OK (max %d subscribers)", EVENT_BUS_MAX_SUBSCRIBERS);
    return ESP_OK;
}

esp_err_t event_bus_publish(const nb_event_t *evt) {
    if (!s_initialized || !evt) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(EVENT_BUS_PUBLISH_TIMEOUT_MS)) != pdTRUE) {
        NB_LOGW(TAG, "publish timeout (type=%d)", evt->type);
        return ESP_ERR_TIMEOUT;
    }

    for (int i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; i++) {
        if (s_subs[i].active && s_subs[i].type == evt->type) {
            if (xQueueSend(s_subs[i].queue, evt, 0) != pdTRUE) {
                NB_LOGW(TAG, "queue cheia para type=%d (subscriber %d)", evt->type, i);
            }
        }
    }

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t event_bus_subscribe(nb_event_type_t type, QueueHandle_t queue) {
    if (!s_initialized || !queue) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;

    esp_err_t ret = ESP_ERR_NO_MEM;
    for (int i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; i++) {
        if (!s_subs[i].active) {
            s_subs[i].type   = type;
            s_subs[i].queue  = queue;
            s_subs[i].active = true;
            ret = ESP_OK;
            break;
        }
    }

    xSemaphoreGive(s_mutex);
    if (ret != ESP_OK) NB_LOGE(TAG, "sem slot para subscriber (type=%d)", type);
    return ret;
}

esp_err_t event_bus_unsubscribe(nb_event_type_t type, QueueHandle_t queue) {
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;

    for (int i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; i++) {
        if (s_subs[i].active && s_subs[i].type == type && s_subs[i].queue == queue) {
            s_subs[i].active = false;
            break;
        }
    }

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}
