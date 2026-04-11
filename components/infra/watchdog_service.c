#include "watchdog_service.h"
#include "logger.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "watchdog";

typedef struct {
    char     name[16];
    uint32_t interval_ms;
    uint32_t last_heartbeat_ms;
    bool     active;
} nb_sw_watchdog_t;

static nb_sw_watchdog_t  s_services[WATCHDOG_MAX_SERVICES];
static SemaphoreHandle_t s_mutex;

static void prv_monitor_task(void *arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) != pdTRUE) continue;
        for (int i = 0; i < WATCHDOG_MAX_SERVICES; i++) {
            if (!s_services[i].active) continue;
            uint32_t elapsed = now - s_services[i].last_heartbeat_ms;
            if (elapsed > s_services[i].interval_ms) {
                NB_LOGW(TAG, "HEARTBEAT AUSENTE: %s (%lums sem sinal)",
                        s_services[i].name, (unsigned long)elapsed);
            }
        }
        xSemaphoreGive(s_mutex);
    }
}

esp_err_t watchdog_service_init(void) {
    memset(s_services, 0, sizeof(s_services));
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    xTaskCreatePinnedToCore(prv_monitor_task, "nb_wdog", 2048, NULL, 9, NULL, 1);
    NB_LOGI(TAG, "Watchdog service OK");
    return ESP_OK;
}

esp_err_t watchdog_register(const char *name, uint32_t interval_ms) {
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t ret = ESP_ERR_NO_MEM;
    for (int i = 0; i < WATCHDOG_MAX_SERVICES; i++) {
        if (!s_services[i].active) {
            strncpy(s_services[i].name, name, sizeof(s_services[i].name) - 1);
            s_services[i].interval_ms        = interval_ms;
            s_services[i].last_heartbeat_ms  = pdTICKS_TO_MS(xTaskGetTickCount());
            s_services[i].active             = true;
            ret = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return ret;
}

esp_err_t watchdog_heartbeat(const char *name) {
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return ESP_ERR_TIMEOUT;
    for (int i = 0; i < WATCHDOG_MAX_SERVICES; i++) {
        if (s_services[i].active && strcmp(s_services[i].name, name) == 0) {
            s_services[i].last_heartbeat_ms = pdTICKS_TO_MS(xTaskGetTickCount());
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t watchdog_unregister(const char *name) {
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    for (int i = 0; i < WATCHDOG_MAX_SERVICES; i++) {
        if (s_services[i].active && strcmp(s_services[i].name, name) == 0) {
            s_services[i].active = false;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t watchdog_add_current_task_to_hw_wdt(void) {
    return esp_task_wdt_add(NULL);
}

void watchdog_feed_hw(void) {
    esp_task_wdt_reset();
}
