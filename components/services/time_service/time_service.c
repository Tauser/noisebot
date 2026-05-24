/*
 * time_service.c — SNTP + fuso horário configurável (Layer 5)
 */

#include "time_service.h"
#include "event_bus.h"
#include "nb_events.h"
#include "logger.h"

#include "freertos/FreeRTOS.h"
#include "esp_sntp.h"
#include "nvs.h"
#include "esp_log.h"

#include <string.h>
#include <stdlib.h>
#include <sys/time.h>

#define TAG "nb_time"

/* ── NVS ─────────────────────────────────────────────────────────────────── */

#define NVS_NS           "nb_time"
#define NVS_KEY_TZ_POSIX "tz_posix"
#define NVS_KEY_TZ_NAME  "tz_name"
#define NVS_KEY_LOCATION "location"

/* ── Defaults ────────────────────────────────────────────────────────────── */

#define DEFAULT_TZ_POSIX  "BRT3"
#define DEFAULT_TZ_NAME   "America/Sao_Paulo"
#define DEFAULT_LOCATION  "Brasilia, DF"

/* ── Estado ──────────────────────────────────────────────────────────────── */

static struct {
    bool initialized;
    bool sntp_started;
    bool synced;
    char tz_posix[NB_TIME_TZ_POSIX_MAX];
    char tz_name[NB_TIME_TZ_NAME_MAX];
    char location[NB_TIME_LOCATION_MAX];
} s;

/* ── SNTP callback (chamado da task LWIP) ────────────────────────────────── */

static void on_sntp_sync(struct timeval *tv)
{
    (void)tv;
    s.synced = true;
    NB_LOGI(TAG, "SNTP sincronizado");
    nb_event_t evt = { .type = NB_EVT_TIME_SYNCED };
    nb_event_publish_async(&evt);
}

/* ── NVS helpers ─────────────────────────────────────────────────────────── */

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    size_t sz;
    sz = sizeof(s.tz_posix);
    if (nvs_get_str(h, NVS_KEY_TZ_POSIX, s.tz_posix, &sz) != ESP_OK)
        strlcpy(s.tz_posix, DEFAULT_TZ_POSIX, sizeof(s.tz_posix));

    sz = sizeof(s.tz_name);
    if (nvs_get_str(h, NVS_KEY_TZ_NAME, s.tz_name, &sz) != ESP_OK)
        strlcpy(s.tz_name, DEFAULT_TZ_NAME, sizeof(s.tz_name));

    sz = sizeof(s.location);
    if (nvs_get_str(h, NVS_KEY_LOCATION, s.location, &sz) != ESP_OK)
        strlcpy(s.location, DEFAULT_LOCATION, sizeof(s.location));

    nvs_close(h);
}

static void nvs_save_tz(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_TZ_POSIX, s.tz_posix);
    nvs_set_str(h, NVS_KEY_TZ_NAME,  s.tz_name);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_location(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_LOCATION, s.location);
    nvs_commit(h);
    nvs_close(h);
}

/* ── Subscriber WiFi ─────────────────────────────────────────────────────── */

static void on_wifi_ip(const nb_event_t *evt, void *ctx)
{
    (void)evt; (void)ctx;
    time_service_sync_now();
}

/* ── API ─────────────────────────────────────────────────────────────────── */

esp_err_t time_service_init(void)
{
    if (s.initialized) return ESP_ERR_INVALID_STATE;

    strlcpy(s.tz_posix, DEFAULT_TZ_POSIX, sizeof(s.tz_posix));
    strlcpy(s.tz_name,  DEFAULT_TZ_NAME,  sizeof(s.tz_name));
    strlcpy(s.location, DEFAULT_LOCATION, sizeof(s.location));

    nvs_load();

    setenv("TZ", s.tz_posix, 1);
    tzset();

    nb_event_subscribe(NB_EVT_WIFI_IP_ACQUIRED, on_wifi_ip, NULL, NULL);

    s.initialized = true;
    NB_LOGI(TAG, "time_service init — TZ=%s (%s) local=%s",
            s.tz_posix, s.tz_name, s.location);
    return ESP_OK;
}

bool time_service_is_synced(void)
{
    return s.synced;
}

esp_err_t time_service_get_local_time(struct tm *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    time_t now;
    time(&now);
    localtime_r(&now, out);
    return ESP_OK;
}

const char *time_service_get_tz_posix(void) { return s.tz_posix; }
const char *time_service_get_tz_name(void)  { return s.tz_name;  }
const char *time_service_get_location(void) { return s.location; }

esp_err_t time_service_set_timezone(const char *posix_tz, const char *tz_name)
{
    if (!posix_tz || !tz_name) return ESP_ERR_INVALID_ARG;
    strlcpy(s.tz_posix, posix_tz, sizeof(s.tz_posix));
    strlcpy(s.tz_name,  tz_name,  sizeof(s.tz_name));
    setenv("TZ", s.tz_posix, 1);
    tzset();
    nvs_save_tz();
    NB_LOGI(TAG, "timezone alterado: %s (%s)", s.tz_posix, s.tz_name);
    return ESP_OK;
}

esp_err_t time_service_set_location(const char *location)
{
    if (!location) return ESP_ERR_INVALID_ARG;
    strlcpy(s.location, location, sizeof(s.location));
    nvs_save_location();
    return ESP_OK;
}

esp_err_t time_service_sync_now(void)
{
    if (!s.initialized) return ESP_ERR_INVALID_STATE;

    if (s.sntp_started) {
        esp_sntp_restart();
        NB_LOGI(TAG, "SNTP restart");
        return ESP_OK;
    }

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(on_sntp_sync);
    esp_sntp_init();
    s.sntp_started = true;
    NB_LOGI(TAG, "SNTP iniciado → pool.ntp.org");
    return ESP_OK;
}
