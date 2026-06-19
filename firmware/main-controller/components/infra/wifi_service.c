/*
 * wifi_service.c — WiFi Service (Layer 2, Etapa 9.6)
 */

#include "wifi_service.h"
#include "nb_config_keys.h"
#if __has_include("wifi_creds.h")
#include "wifi_creds.h"
#endif
#ifndef NB_WIFI_DEV_SSID
#define NB_WIFI_DEV_SSID ""
#define NB_WIFI_DEV_PASS ""
#endif
#include "nb_events.h"
#include "event_bus.h"
#include "logger.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <string.h>
#include <stdio.h>

#define TAG             "nb_wifi"
#define MDNS_HOSTNAME   "noisebot"

#define BACKOFF_MIN_US   1000000LL    /*  1s */
#define BACKOFF_MAX_US  60000000LL    /* 60s */
#define HEALTH_PERIOD_US 30000000LL    /* 30s */

/* ── Estado ──────────────────────────────────────────────────────────────── */

static bool               s_initialized    = false;
static bool               s_enabled        = false;
static bool               s_connected      = false;
static char               s_ip[16]         = "";
static char               s_ssid[33]       = "";
static uint16_t           s_last_disc_reason = 0U;
static uint32_t           s_disconnect_count = 0U;
static portMUX_TYPE       s_mux            = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_reconnect_tmr  = NULL;
static esp_timer_handle_t s_health_tmr     = NULL;
static int64_t            s_backoff_us     = BACKOFF_MIN_US;

/* ── Reconexão com backoff ────────────────────────────────────────────────── */

static void reconnect_cb(void *arg)
{
    (void)arg;
    if (!s_enabled) return;
    NB_LOGI(TAG, "tentando reconectar...");
    esp_wifi_connect();
}

static void schedule_reconnect(void)
{
    if (!s_enabled) return;
    if (!s_reconnect_tmr) return;
    esp_timer_stop(s_reconnect_tmr);
    NB_LOGI(TAG, "próxima tentativa em %.1fs", (double)s_backoff_us / 1e6);
    esp_timer_start_once(s_reconnect_tmr, s_backoff_us);
    s_backoff_us = s_backoff_us * 2;
    if (s_backoff_us > BACKOFF_MAX_US) s_backoff_us = BACKOFF_MAX_US;
}

static void health_cb(void *arg)
{
    (void)arg;
    if (!s_initialized || !s_enabled) return;
    if (wifi_service_is_connected()) return;

    bool reconnect_active = false;
    if (s_reconnect_tmr) {
        reconnect_active = esp_timer_is_active(s_reconnect_tmr);
    }
    if (!reconnect_active) {
        NB_LOGW(TAG, "watchdog WiFi: desconectado sem timer ativo — reagendando");
        schedule_reconnect();
    }
}

/* ── Handlers de eventos WiFi ────────────────────────────────────────────── */

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    (void)arg; (void)base;

    switch ((wifi_event_t)id) {
        case WIFI_EVENT_STA_START:
            NB_LOGI(TAG, "STA start — conectando");
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_CONNECTED:
            NB_LOGI(TAG, "associado ao AP (aguardando DHCP)");
            /* Desabilita WiFi power management (light sleep).
             * CONFIG_SOC_WIFI_PHY_NEEDS_USB_WORKAROUND reativa usb_pad_enable
             * durante micro-sleeps do PM type=1, corrompendo UART1 RX (GPIO 19).
             * Com fonte 5V/3A não há restrição de energia — PM desativado. */
            esp_wifi_set_ps(WIFI_PS_NONE);
            {
                nb_event_t e = { .type = NB_EVT_WIFI_CONNECTED };
                nb_event_publish_async(&e);
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            bool was_connected;
            taskENTER_CRITICAL(&s_mux);
            was_connected = s_connected;
            s_connected   = false;
            s_ip[0]       = '\0';
            taskEXIT_CRITICAL(&s_mux);

            wifi_event_sta_disconnected_t *ev =
                (wifi_event_sta_disconnected_t *)data;
            uint16_t reason = ev ? (uint16_t)ev->reason : 0U;
            uint32_t disconnect_count;
            taskENTER_CRITICAL(&s_mux);
            s_last_disc_reason = reason;
            s_disconnect_count++;
            disconnect_count = s_disconnect_count;
            taskEXIT_CRITICAL(&s_mux);
            NB_LOGW(TAG, "desconectado (reason=%u, count=%lu)",
                    (unsigned)reason,
                    (unsigned long)disconnect_count);

            if (was_connected) {
                nb_event_t e = { .type = NB_EVT_WIFI_DISCONNECTED };
                nb_event_publish_async(&e);
            }
            schedule_reconnect();
            break;
        }
        default: break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    (void)arg; (void)base;

    if ((ip_event_t)id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;

        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ev->ip_info.ip));

        taskENTER_CRITICAL(&s_mux);
        s_connected = true;
        strlcpy(s_ip, ip_str, sizeof(s_ip));
        taskEXIT_CRITICAL(&s_mux);

        s_backoff_us = BACKOFF_MIN_US;   /* reset ao obter IP */

        NB_LOGI(TAG, "IP: %s — API HTTP local ativa", ip_str);

        nb_event_t e = { .type = NB_EVT_WIFI_IP_ACQUIRED };
        nb_event_publish_async(&e);

        mdns_hostname_set(MDNS_HOSTNAME);
        mdns_instance_name_set("NoiseBot");
    }
}

/* ── API pública ─────────────────────────────────────────────────────────── */

esp_err_t wifi_service_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;

    /* Ler credenciais do NVS. */
    char ssid[33] = "";
    char pass[65] = "";

    nvs_handle_t nvs;
    if (nvs_open(NB_WIFI_NS, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t enabled = 1;
        nvs_get_u8(nvs, NB_WIFI_KEY_EN, &enabled);
        if (!enabled) {
            nvs_close(nvs);
            NB_LOGI(TAG, "WiFi desabilitado via NVS — offline");
            return ESP_OK;
        }
        size_t ssid_len = sizeof(ssid);
        size_t pass_len = sizeof(pass);
        nvs_get_str(nvs, NB_WIFI_KEY_SSID, ssid, &ssid_len);
        nvs_get_str(nvs, NB_WIFI_KEY_PASS, pass, &pass_len);
        nvs_close(nvs);
    }

    /* Credenciais de desenvolvimento (fallback quando NVS vazio).
     * Defina NB_WIFI_DEV_SSID/NB_WIFI_DEV_PASS via cmake ou nb_config_keys.h. */
    if (ssid[0] == '\0' && NB_WIFI_DEV_SSID[0] != '\0') {
        strlcpy(ssid, NB_WIFI_DEV_SSID, sizeof(ssid));
        strlcpy(pass, NB_WIFI_DEV_PASS, sizeof(pass));
        NB_LOGI(TAG, "WiFi: credenciais de compilação (dev)");
    }

    strlcpy(s_ssid, ssid, sizeof(s_ssid));

    if (ssid[0] == '\0') {
        NB_LOGW(TAG, "WiFi sem credenciais — offline");
        return ESP_OK;
    }
    s_enabled = true;

    /* Infraestrutura de rede (idempotente). */
    esp_netif_init();
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        NB_LOGE(TAG, "event_loop_create: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    if (!netif) {
        NB_LOGE(TAG, "esp_netif_create_default_wifi_sta falhou");
        return ESP_FAIL;
    }

    /* Inicializa driver WiFi. */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        NB_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        return err;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        on_wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        on_ip_event, NULL, NULL);

    /* Credenciais. */
    wifi_config_t wifi_cfg = { 0 };
    strlcpy((char *)wifi_cfg.sta.ssid,     ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WEP;  /* aceita WPA/WPA2/WPA3 */

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);

    /* Timer de reconexão (one-shot, rearmado a cada disconnect). */
    const esp_timer_create_args_t tmr = {
        .callback = reconnect_cb,
        .name     = "wifi_reconnect",
    };
    esp_timer_create(&tmr, &s_reconnect_tmr);

    const esp_timer_create_args_t health_tmr = {
        .callback = health_cb,
        .name     = "wifi_health",
    };
    esp_timer_create(&health_tmr, &s_health_tmr);

    /* mDNS (hostname definido depois do IP). */
    err = mdns_init();
    if (err != ESP_OK) {
        NB_LOGW(TAG, "mdns_init: %s — noisebot.local indisponivel",
                esp_err_to_name(err));
    }

    /* Inicia WiFi — conexão ocorre via WIFI_EVENT_STA_START em background. */
    err = esp_wifi_start();
    if (err != ESP_OK) {
        NB_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    if (s_health_tmr) {
        esp_timer_start_periodic(s_health_tmr, HEALTH_PERIOD_US);
    }
    NB_LOGI(TAG, "WiFi init OK — conectando a '%s'", ssid);
    return ESP_OK;
}

bool wifi_service_is_connected(void)
{
    bool result;
    taskENTER_CRITICAL(&s_mux);
    result = s_connected;
    taskEXIT_CRITICAL(&s_mux);
    return result;
}

const char *wifi_service_get_ip(void)
{
    return s_ip;
}

const char *wifi_service_get_ssid(void)
{
    return s_ssid;
}

int8_t wifi_service_get_rssi(void)
{
    if (!s_connected) return 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

uint16_t wifi_service_get_last_disconnect_reason(void)
{
    uint16_t result;
    taskENTER_CRITICAL(&s_mux);
    result = s_last_disc_reason;
    taskEXIT_CRITICAL(&s_mux);
    return result;
}

uint32_t wifi_service_get_disconnect_count(void)
{
    uint32_t result;
    taskENTER_CRITICAL(&s_mux);
    result = s_disconnect_count;
    taskEXIT_CRITICAL(&s_mux);
    return result;
}

esp_err_t wifi_service_set_credentials(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NB_WIFI_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    nvs_set_str(nvs, NB_WIFI_KEY_SSID, ssid);
    nvs_set_str(nvs, NB_WIFI_KEY_PASS, pass ? pass : "");
    uint8_t enabled = 1;
    nvs_set_u8(nvs, NB_WIFI_KEY_EN, enabled);
    nvs_commit(nvs);
    nvs_close(nvs);

    taskENTER_CRITICAL(&s_mux);
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    taskEXIT_CRITICAL(&s_mux);
    s_enabled = true;

    if (s_initialized) {
        wifi_config_t cfg = { 0 };
        strlcpy((char *)cfg.sta.ssid,     ssid,         sizeof(cfg.sta.ssid));
        strlcpy((char *)cfg.sta.password, pass ? pass : "", sizeof(cfg.sta.password));
        cfg.sta.threshold.authmode = WIFI_AUTH_WEP;
        esp_wifi_disconnect();
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
        s_backoff_us = BACKOFF_MIN_US;
        esp_wifi_connect();
        NB_LOGI(TAG, "credenciais atualizadas — reconectando a '%s'", ssid);
    }
    return ESP_OK;
}

esp_err_t wifi_service_clear_credentials(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NB_WIFI_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, NB_WIFI_KEY_SSID);
        nvs_erase_key(nvs, NB_WIFI_KEY_PASS);
        uint8_t disabled = 0;
        nvs_set_u8(nvs, NB_WIFI_KEY_EN, disabled);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (s_initialized) {
        s_enabled = false;
        esp_wifi_disconnect();
        esp_timer_stop(s_reconnect_tmr);
    }
    taskENTER_CRITICAL(&s_mux);
    s_ssid[0]     = '\0';
    s_connected   = false;
    s_ip[0]       = '\0';
    taskEXIT_CRITICAL(&s_mux);
    NB_LOGI(TAG, "credenciais WiFi limpas — offline");
    return ESP_OK;
}
