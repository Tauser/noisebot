/*
 * web_service.c — Web Dashboard e Companion API (Layer 2, Etapas 15.1–15.6)
 */

#include "web_service.h"
#include "nb_events.h"
#include "event_bus.h"
#include "logger.h"
#include "state_machine.h"
#include "emotion_model.h"
#include "attention_service.h"
#include "diagnostics_service.h"
#include "persona_service.h"
#include "config_manager.h"
#include "conductor.h"
#include "expression_service.h"
#include "gaze_service.h"
#include "circadian_service.h"
#include "idle_service.h"
#include "motion_service.h"
#include "servo_hal.h"
#include "conductor.h"
#include "rhythm_service.h"
#include "sound_analysis_service.h"
#include "wifi_service.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "led_service.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "nvs_hal.h"
#include "nvs.h"
#include "long_term_memory.h"
#include "nb_config_keys.h"
#include "bridge_service.h"
#include "wake_service.h"
#include "audio_service.h"
#include "touch_service.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define TAG              "nb_web"
#define SD_WWW_INDEX     "/sdcard/www/index.html"
#define SD_WWW_JS        "/sdcard/www/app.js"
#define SD_WWW_CSS       "/sdcard/www/style.css"
#define MAX_BODY_LEN     512

/* ── Log ring buffer ─────────────────────────────────────────────────────── */

#define LOG_RING_SIZE  32
#define LOG_LINE_MAX   120

static char           s_log_ring[LOG_RING_SIZE][LOG_LINE_MAX];
static uint32_t       s_log_head  = 0;
static uint32_t       s_log_count = 0;
static portMUX_TYPE   s_log_mux   = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig_vprintf = NULL;

static int log_hook_vprintf(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int ret = s_orig_vprintf(fmt, args);

    taskENTER_CRITICAL(&s_log_mux);
    uint32_t slot = s_log_head;
    s_log_head = (s_log_head + 1) % LOG_RING_SIZE;
    if (s_log_count < LOG_RING_SIZE) s_log_count++;
    taskEXIT_CRITICAL(&s_log_mux);

    vsnprintf(s_log_ring[slot], LOG_LINE_MAX, fmt, copy);
    va_end(copy);
    return ret;
}

/* ── Estado ──────────────────────────────────────────────────────────────── */

static httpd_handle_t     s_server     = NULL;
static int                s_ws_fd      = -1;
static portMUX_TYPE       s_mux        = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_audio_tmr       = NULL;
static esp_timer_handle_t s_servo_calib_tmr = NULL;  /* auto-resume conductor após calibração */
static nb_event_type_t    s_last_touch_event = NB_EVT_NONE;
static int64_t            s_last_touch_us    = 0;

/* ── Tabelas de nomes ────────────────────────────────────────────────────── */

static const char *const k_state_names[] = {
    "BOOT_UP", "IDLE", "ATTENTIVE", "RESPONDING", "TOUCH_REACTING",
    "SLEEPING", "ERROR", "SAFE_MODE", "MEDITATION", "SILENT_COMPANY"
};

static const char *const k_expr_names[] = {
    "NEUTRAL", "HAPPY", "CURIOUS", "SLEEPY", "FOCUSED",
    "SUSPICIOUS", "SURPRISED", "SAD", "ALARMED", "ANGRY"
};

static const char *const k_circadian_names[] = { "DAWN", "DAY", "DUSK" };

static const char *const k_emot_evt_names[] = {
    "TOUCH_TAP", "TOUCH_LONG", "VOICE_START", "AUDIO_STARTED",
    "ENTERING_SLEEP", "WAKING_UP", "MOTION_FAULT", "IDLE_LONG",
    "VOICE_LOUD", "VOICE_SOFT", "TOUCH_WARM_PULSE", "TOUCH_DEEP", "TOUCH_CARESS"
};

static nb_expression_t expr_from_str(const char *s)
{
    if (!s) return NB_EXPR_NEUTRAL;
    for (int i = 0; i < NB_EXPR_COUNT; i++)
        if (strcmp(s, k_expr_names[i]) == 0) return (nb_expression_t)i;
    return NB_EXPR_NEUTRAL;
}

static void json_escape(const char *src, char *dst, size_t dstlen)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dstlen; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') { dst[j++] = '\\'; dst[j++] = (char)c; }
        else if (c == '\n')        { dst[j++] = '\\'; dst[j++] = 'n'; }
        else if (c == '\r')        { /* skip CR */ }
        else if (c < 0x20)         { /* skip other control chars */ }
        else                       { dst[j++] = (char)c; }
    }
    dst[j] = '\0';
}

static const char *state_name(nb_robot_state_t s)
{
    if ((unsigned)s < sizeof(k_state_names) / sizeof(k_state_names[0]))
        return k_state_names[(unsigned)s];
    return "UNKNOWN";
}

static const char *expr_name(nb_expression_t e)
{
    if ((unsigned)e < sizeof(k_expr_names) / sizeof(k_expr_names[0]))
        return k_expr_names[(unsigned)e];
    return "UNKNOWN";
}

static const char *touch_state_name(nb_touch_state_t state)
{
    switch (state) {
        case NB_TOUCH_STATE_IDLE:             return "IDLE";
        case NB_TOUCH_STATE_TOUCHING:         return "TOUCHING";
        case NB_TOUCH_STATE_LONG_PRESSING:    return "LONG_PRESSING";
        case NB_TOUCH_STATE_SUSTAINED_ACTIVE: return "SUSTAINED_ACTIVE";
        default:                              return "UNKNOWN";
    }
}

static const char *touch_event_name(nb_event_type_t event)
{
    switch (event) {
        case NB_EVT_TOUCH_TAP:          return "TAP";
        case NB_EVT_TOUCH_LONG_PRESS:   return "LONG_PRESS";
        case NB_EVT_TOUCH_SUSTAINED:    return "SUSTAINED";
        case NB_EVT_TOUCH_WAKE:         return "WAKE";
        case NB_EVT_TOUCH_DOUBLE_TAP:   return "DOUBLE_TAP";
        case NB_EVT_TOUCH_DEEP:         return "DEEP";
        case NB_EVT_TOUCH_CARESS:       return "CARESS";
        case NB_EVT_TOUCH_WARM_PULSE:   return "WARM_PULSE";
        default:                        return "NONE";
    }
}

/* ── Construção de JSON de status ────────────────────────────────────────── */

static void build_status_json(char *buf, size_t size)
{
    nb_robot_state_t st  = state_machine_get_state();
    nb_expression_t  ex  = emotion_model_get_expression();
    float            att = attention_service_get_level();
    uint8_t          hlt = diagnostics_get_health_score();
    uint32_t         upt = diagnostics_get_uptime_s();
    float            fps = diagnostics_get_fps();

    snprintf(buf, size,
        "{\"state\":\"%s\",\"expression\":\"%s\","
        "\"attention\":%.2f,\"health\":%u,\"uptime_s\":%lu,\"fps\":%.1f}",
        state_name(st), expr_name(ex), (double)att,
        (unsigned)hlt, (unsigned long)upt, (double)fps);
}

/* ── Push WebSocket ──────────────────────────────────────────────────────── */

static void ws_push_json(const char *json)
{
    int fd;
    taskENTER_CRITICAL(&s_mux);
    fd = s_ws_fd;
    taskEXIT_CRITICAL(&s_mux);

    if (!s_server || fd < 0) return;

    httpd_ws_frame_t frame = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len     = strlen(json),
        .final   = true,
    };
    esp_err_t err = httpd_ws_send_frame_async(s_server, fd, &frame);
    if (err != ESP_OK) {
        taskENTER_CRITICAL(&s_mux);
        if (s_ws_fd == fd) s_ws_fd = -1;
        taskEXIT_CRITICAL(&s_mux);
    }
}

static void ws_push_status(void)
{
    char json[256];
    build_status_json(json, sizeof(json));
    ws_push_json(json);
}

static void ws_push_ota(int pct, const char *status, const char *msg)
{
    char json[192];
    if (msg)
        snprintf(json, sizeof(json),
            "{\"type\":\"ota\",\"progress\":%d,\"status\":\"%s\",\"message\":\"%s\"}",
            pct, status, msg);
    else
        snprintf(json, sizeof(json),
            "{\"type\":\"ota\",\"progress\":%d,\"status\":\"%s\"}",
            pct, status);
    ws_push_json(json);
}

static void ws_push_persona(void)
{
    char json[128];
    snprintf(json, sizeof(json),
        "{\"type\":\"persona_update\","
        "\"warmth\":%.3f,\"energy\":%.3f,\"curiosity\":%.3f,\"trust\":%.3f}",
        (double)persona_get_warmth(), (double)persona_get_energy(),
        (double)persona_get_curiosity(), (double)persona_get_trust());
    ws_push_json(json);
}

static void on_persona_refreshed(const nb_event_t *evt, void *ctx)
{
    (void)evt; (void)ctx;
    ws_push_persona();
}

static void audio_push_timer_cb(void *arg)
{
    (void)arg;
    int fd;
    taskENTER_CRITICAL(&s_mux);
    fd = s_ws_fd;
    taskEXIT_CRITICAL(&s_mux);
    if (!s_server || fd < 0) return;

    nb_emotion_vec_t vec   = emotion_model_get_vec();
    nb_circadian_phase_t ph = circadian_get_phase();
    float bpm              = rhythm_service_get_bpm();
    float bpm_conf         = rhythm_service_get_confidence();
    float rms              = sound_analysis_get_rms();

    char json[256];
    snprintf(json, sizeof(json),
        "{\"type\":\"telemetry\","
        "\"valence\":%.3f,\"activation\":%.3f,"
        "\"circadian\":\"%s\","
        "\"bpm\":%.1f,\"bpm_conf\":%.2f,\"rms\":%.3f}",
        (double)vec.valence, (double)vec.activation,
        k_circadian_names[(int)ph < 3 ? (int)ph : 1],
        (double)bpm, (double)bpm_conf, (double)rms);
    ws_push_json(json);
}

/* ── Dashboard embutido (EMBED_TXTFILES) ─────────────────────────────────── */

extern const uint8_t dashboard_html_start[] asm("_binary_dashboard_html_start");
extern const uint8_t dashboard_html_end[]   asm("_binary_dashboard_html_end");

/* ── Handlers HTTP ───────────────────────────────────────────────────────── */

static esp_err_t serve_file_or_fallback(httpd_req_t *req,
                                         const char *path,
                                         const char *mime,
                                         const char *fallback,
                                         size_t      fallback_len)
{
    FILE *f = fopen(path, "r");
    httpd_resp_set_type(req, mime);
    if (f) {
        char chunk[512];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
            if (httpd_resp_send_chunk(req, chunk, (ssize_t)n) != ESP_OK) break;
        }
        fclose(f);
        httpd_resp_send_chunk(req, NULL, 0);
    } else {
        httpd_resp_send(req, fallback, (ssize_t)fallback_len);
    }
    return ESP_OK;
}

static esp_err_t handle_root(httpd_req_t *req)
{
    size_t embedded_len = (size_t)(dashboard_html_end - dashboard_html_start);
    return serve_file_or_fallback(req, SD_WWW_INDEX, "text/html",
                                  (const char *)dashboard_html_start, embedded_len);
}

static esp_err_t handle_app_js(httpd_req_t *req)
{
    FILE *f = fopen(SD_WWW_JS, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "app.js not found");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/javascript");
    char chunk[512];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        if (httpd_resp_send_chunk(req, chunk, (ssize_t)n) != ESP_OK) break;
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_style_css(httpd_req_t *req)
{
    FILE *f = fopen(SD_WWW_CSS, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "style.css not found");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/css");
    char chunk[512];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        if (httpd_resp_send_chunk(req, chunk, (ssize_t)n) != ESP_OK) break;
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_api_status(httpd_req_t *req)
{
    char buf[256];
    build_status_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_persona(httpd_req_t *req)
{
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"warmth\":%.3f,\"energy\":%.3f,\"curiosity\":%.3f,\"trust\":%.3f}",
        (double)persona_get_warmth(), (double)persona_get_energy(),
        (double)persona_get_curiosity(), (double)persona_get_trust());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_config_get(httpd_req_t *req)
{
    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"volume\":%u,\"brightness\":%u,\"touch_sens\":%u,"
        "\"idle_timeout\":%lu,"
        "\"srv1_min\":%d,\"srv1_max\":%d,\"srv1_ctr\":%d,"
        "\"srv2_min\":%d,\"srv2_max\":%d,\"srv2_ctr\":%d}",
        (unsigned)config_get_volume(),
        (unsigned)config_get_brightness(),
        (unsigned)config_get_touch_sensitivity(),
        (unsigned long)config_get_idle_timeout_s(),
        (int)config_get_servo_limit_min(1),
        (int)config_get_servo_limit_max(1),
        (int)config_get_servo_center(1),
        (int)config_get_servo_limit_min(2),
        (int)config_get_servo_limit_max(2),
        (int)config_get_servo_center(2));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static bool recv_body(httpd_req_t *req, char *buf, size_t size, int *out_len)
{
    int total = (int)req->content_len;
    if (total <= 0 || total >= (int)size) return false;

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, (size_t)(total - received));
        if (r <= 0) return false;
        received += r;
    }
    buf[received] = '\0';
    if (out_len) *out_len = received;
    return true;
}

static esp_err_t handle_api_config_post(httpd_req_t *req)
{
    char body[MAX_BODY_LEN];
    if (!recv_body(req, body, sizeof(body), NULL)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength(body, strlen(body));
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_OK;
    }

    const cJSON *key_j = cJSON_GetObjectItemCaseSensitive(root, "key");
    const cJSON *val_j = cJSON_GetObjectItemCaseSensitive(root, "value");

    if (!cJSON_IsString(key_j) || !cJSON_IsNumber(val_j)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing key or value");
        return ESP_OK;
    }

    const char *key = key_j->valuestring;
    double      val = val_j->valuedouble;
    esp_err_t   err = ESP_ERR_NOT_FOUND;

    if      (strcmp(key, "volume")       == 0) err = config_set_volume((uint8_t)val);
    else if (strcmp(key, "brightness")   == 0) err = config_set_brightness((uint8_t)val);
    else if (strcmp(key, "touch_sens")   == 0) err = config_set_touch_sensitivity((uint8_t)val);
    else if (strcmp(key, "idle_timeout") == 0) err = config_set_idle_timeout_s((uint32_t)val);
    else if (strcmp(key, "srv1_min")     == 0) err = config_set_servo_limit_min(1, (int16_t)val);
    else if (strcmp(key, "srv1_max")     == 0) err = config_set_servo_limit_max(1, (int16_t)val);
    else if (strcmp(key, "srv1_ctr")     == 0) err = config_set_servo_center(1, (int16_t)val);
    else if (strcmp(key, "srv2_min")     == 0) err = config_set_servo_limit_min(2, (int16_t)val);
    else if (strcmp(key, "srv2_max")     == 0) err = config_set_servo_limit_max(2, (int16_t)val);
    else if (strcmp(key, "srv2_ctr")     == 0) err = config_set_servo_center(2, (int16_t)val);

    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
        NB_LOGI(TAG, "config '%s' atualizado via API", key);
    } else {
        char errb[64];
        snprintf(errb, sizeof(errb), "{\"ok\":false,\"error\":\"%s\"}",
                 esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, errb);
    }
    return ESP_OK;
}

static nb_action_t action_from_str(const char *s)
{
    if (!s) return NB_ACTION_NONE;
    if (strcmp(s, "GREET")         == 0) return NB_ACTION_GREET;
    if (strcmp(s, "AGREE")         == 0) return NB_ACTION_AGREE;
    if (strcmp(s, "DISAGREE")      == 0) return NB_ACTION_DISAGREE;
    if (strcmp(s, "CURIOUS")       == 0) return NB_ACTION_CURIOUS;
    if (strcmp(s, "TOUCH_WARM")    == 0) return NB_ACTION_TOUCH_WARM;
    if (strcmp(s, "TOUCH_STARTLE") == 0) return NB_ACTION_TOUCH_STARTLE;
    if (strcmp(s, "SPEAK_LOOP")    == 0) return NB_ACTION_SPEAK_LOOP;
    if (strcmp(s, "SLEEP")         == 0) return NB_ACTION_SLEEP;
    if (strcmp(s, "WAKE_UP")       == 0) return NB_ACTION_WAKE_UP;
    if (strcmp(s, "STRETCH")       == 0) return NB_ACTION_STRETCH;
    if (strcmp(s, "CELEBRATE")     == 0) return NB_ACTION_CELEBRATE;
    return NB_ACTION_NONE;
}

/* Callback do timer: retoma o conductor após 10s sem comandos de calibração */
static void servo_calib_resume_cb(void *arg)
{
    (void)arg;
    conductor_pause(false);
    NB_LOGI(TAG, "calibracao servo: conductor retomado (timeout)");
}

/* POST /api/servo — move servo para posição imediata (calibração ao vivo).
 * Body: {"servo": 1, "pos": 512}   servo 1 ou 2, pos em steps [0, 1023]
 *       {"servo": 0, "pos": 0}     servo=0 → park_all (ignora pos)
 * Pausa o conductor automaticamente e retoma após 10s de inatividade.  */
static esp_err_t handle_api_servo_post(httpd_req_t *req)
{
    char body[128];
    if (!recv_body(req, body, sizeof(body), NULL)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_ParseWithLength(body, strlen(body));
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_OK;
    }

    const cJSON *servo_j = cJSON_GetObjectItemCaseSensitive(root, "servo");
    const cJSON *pos_j   = cJSON_GetObjectItemCaseSensitive(root, "pos");

    esp_err_t ret = ESP_ERR_INVALID_ARG;
    if (cJSON_IsNumber(servo_j) && cJSON_IsNumber(pos_j)) {
        int servo = (int)servo_j->valuedouble;
        int pos   = (int)pos_j->valuedouble;

        /* Pausa conductor para evitar override durante calibração */
        conductor_pause(true);
        if (s_servo_calib_tmr) {
            esp_timer_stop(s_servo_calib_tmr);
            esp_timer_start_once(s_servo_calib_tmr, 10000000LL); /* 10s */
        }

        /* Usa servo_hal diretamente — bypass de safety intencional para calibração.
         * GPIO 20 (TX) sofre ~20-30% perda de pacotes com WiFi ativo (USB D- RF).
         * Comandos fire-and-forget: logamos envio, não confirmação do servo.
         * Enviamos cada comando N vezes com gap para garantir entrega estatística.
         * A 30% de perda, P(todas 8 falharem) < 0.001%. */
        #define CAL_SEND(fn, id, ...)  do { \
            for (int _t = 0; _t < 8; _t++) { \
                fn((id), ##__VA_ARGS__); \
                vTaskDelay(pdMS_TO_TICKS(15)); \
            } \
        } while (0)

        if (servo == 0) {
            int16_t c1 = config_get_servo_center(1);
            int16_t c2 = config_get_servo_center(2);
            CAL_SEND(servo_hal_enable_torque,    1u);
            CAL_SEND(servo_hal_enable_torque,    2u);
            CAL_SEND(servo_hal_write_position,   1u, (uint16_t)c1, 600u);
            CAL_SEND(servo_hal_write_position,   2u, (uint16_t)c2, 600u);
            ret = ESP_OK;
        } else if (servo >= 1 && servo <= 2 && pos >= 0 && pos <= 1023) {
            CAL_SEND(servo_hal_enable_torque,    (uint8_t)servo);
            CAL_SEND(servo_hal_write_position,   (uint8_t)servo, (uint16_t)pos, 400u);
            ret = ESP_OK;
        }
        #undef CAL_SEND
    }
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (ret == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"servo cmd failed\"}");
    }
    return ESP_OK;
}

static esp_err_t handle_api_command(httpd_req_t *req)
{
    char body[MAX_BODY_LEN];
    if (!recv_body(req, body, sizeof(body), NULL)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength(body, strlen(body));
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_OK;
    }

    const cJSON *type_j  = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *value_j = cJSON_GetObjectItemCaseSensitive(root, "value");

    bool executed = false;

    if (cJSON_IsString(type_j) &&
        strcmp(type_j->valuestring, "ACTION") == 0 &&
        cJSON_IsString(value_j))
    {
        nb_action_t action = action_from_str(value_j->valuestring);
        if (action != NB_ACTION_NONE) {
            conductor_play(action);
            NB_LOGI(TAG, "command ACTION %s via API", value_j->valuestring);
            executed = true;
        }
    }

    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (executed) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unknown command\"}");
    }
    return ESP_OK;
}

/* ── Handler WebSocket /ws ───────────────────────────────────────────────── */

static void ws_handle_text_frame(const uint8_t *data, size_t len)
{
    cJSON *root = cJSON_ParseWithLength((const char *)data, len);
    if (!root) return;

    const cJSON *type_j = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type_j)) { cJSON_Delete(root); return; }

    const char *type = type_j->valuestring;

    if (strcmp(type, "command") == 0) {
        const cJSON *act_j = cJSON_GetObjectItemCaseSensitive(root, "action");
        if (cJSON_IsString(act_j)) {
            nb_action_t action = action_from_str(act_j->valuestring);
            if (action != NB_ACTION_NONE) {
                conductor_play(action);
                NB_LOGI(TAG, "WS command %s", act_j->valuestring);
            }
        }
    } else if (strcmp(type, "emot_event") == 0) {
        const cJSON *ev_j = cJSON_GetObjectItemCaseSensitive(root, "event");
        if (cJSON_IsString(ev_j)) {
            /* Map string → nb_event_type_t and publish */
            nb_event_type_t et = NB_EVT_NONE;
            const char *ev = ev_j->valuestring;
            if      (strcmp(ev, "TOUCH_TAP")        == 0) et = NB_EVT_TOUCH_TAP;
            else if (strcmp(ev, "TOUCH_LONG_PRESS")  == 0) et = NB_EVT_TOUCH_LONG_PRESS;
            else if (strcmp(ev, "VOICE_START")       == 0) et = NB_EVT_VOICE_ACTIVITY_START;
            else if (strcmp(ev, "VOICE_END")         == 0) et = NB_EVT_VOICE_ACTIVITY_END;
            if (et != NB_EVT_NONE) {
                nb_event_t e = { .type = et };
                nb_event_publish_async(&e);
                NB_LOGI(TAG, "WS emot_event %s", ev);
            }
        }
    }

    cJSON_Delete(root);
}

static esp_err_t handle_ws(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int new_fd = httpd_req_to_sockfd(req);

        int old_fd;
        taskENTER_CRITICAL(&s_mux);
        old_fd   = s_ws_fd;
        s_ws_fd  = new_fd;
        taskEXIT_CRITICAL(&s_mux);

        if (old_fd >= 0 && old_fd != new_fd) {
            NB_LOGI(TAG, "WS: novo cliente fd=%d — desconectando fd=%d", new_fd, old_fd);
            httpd_sess_trigger_close(s_server, old_fd);
        } else {
            NB_LOGI(TAG, "WS: cliente conectado fd=%d", new_fd);
        }

        /* Enviar status inicial ao novo cliente. */
        ws_push_status();
        return ESP_OK;
    }

    /* Receber frame do cliente. */
    httpd_ws_frame_t frame = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = NULL,
        .len     = 0,
    };

    /* Primeiro recv para descobrir o tamanho. */
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;

    if (frame.len == 0) return ESP_OK;

    if (frame.len > MAX_BODY_LEN) {
        NB_LOGW(TAG, "WS frame muito grande (%zu bytes) — ignorado", frame.len);
        return ESP_OK;
    }

    uint8_t *buf = malloc(frame.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret == ESP_OK && frame.type == HTTPD_WS_TYPE_TEXT) {
        buf[frame.len] = '\0';
        ws_handle_text_frame(buf, frame.len);
    }

    free(buf);
    return ret;
}

/* ── OTA (Etapa 15.2) ────────────────────────────────────────────────────── */

typedef struct { char url[256]; } ota_task_arg_t;

static void ota_task(void *arg)
{
    ota_task_arg_t *a = (ota_task_arg_t *)arg;
    char url[256];
    strlcpy(url, a->url, sizeof(url));
    free(a);

    NB_LOGI(TAG, "OTA: iniciando de %s", url);
    led_base_set(NB_LED_BASE_SAFE_MODE, true);
    ws_push_ota(0, "started", NULL);

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        NB_LOGE(TAG, "OTA: nenhuma partição OTA disponível");
        ws_push_ota(0, "error", "no OTA partition");
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_config_t hcfg = {
        .url        = url,
        .timeout_ms = 30000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (!client || esp_http_client_open(client, 0) != ESP_OK) {
        NB_LOGE(TAG, "OTA: http open falhou");
        ws_push_ota(0, "error", "http open failed");
        if (client) esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    int content_len = (int)esp_http_client_fetch_headers(client);

    esp_ota_handle_t ota_handle;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle) != ESP_OK) {
        NB_LOGE(TAG, "OTA: ota_begin falhou");
        ws_push_ota(0, "error", "ota begin failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    uint8_t *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ws_push_ota(0, "error", "no memory");
        vTaskDelete(NULL);
        return;
    }

    int total = 0;
    int last_pct = -1;
    esp_err_t write_err = ESP_OK;

    while (write_err == ESP_OK) {
        int n = esp_http_client_read(client, (char *)buf, 4096);
        if (n < 0) { write_err = ESP_FAIL; break; }
        if (n == 0) break;
        write_err = esp_ota_write(ota_handle, buf, (size_t)n);
        total += n;
        int pct = (content_len > 0) ? (total * 100 / content_len) : 50;
        if (pct > 99) pct = 99;
        if (pct != last_pct) { ws_push_ota(pct, "downloading", NULL); last_pct = pct; }
    }

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (write_err != ESP_OK || total == 0) {
        esp_ota_abort(ota_handle);
        ws_push_ota(0, "error", "download failed");
        vTaskDelete(NULL);
        return;
    }

    if (esp_ota_end(ota_handle) != ESP_OK ||
        esp_ota_set_boot_partition(part) != ESP_OK) {
        NB_LOGE(TAG, "OTA: finalização falhou");
        ws_push_ota(0, "error", "finalization failed");
        vTaskDelete(NULL);
        return;
    }

    NB_LOGI(TAG, "OTA: OK (%d bytes) — reiniciando em 3s", total);
    ws_push_ota(100, "complete", NULL);
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

static esp_err_t handle_api_ota(httpd_req_t *req)
{
    char body[MAX_BODY_LEN];
    if (!recv_body(req, body, sizeof(body), NULL)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_ParseWithLength(body, strlen(body));
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON"); return ESP_OK; }

    const cJSON *url_j = cJSON_GetObjectItemCaseSensitive(root, "url");
    if (!cJSON_IsString(url_j) || url_j->valuestring[0] == '\0') {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing url");
        return ESP_OK;
    }

    ota_task_arg_t *arg = malloc(sizeof(ota_task_arg_t));
    if (arg) strlcpy(arg->url, url_j->valuestring, sizeof(arg->url));
    cJSON_Delete(root);

    if (!arg || xTaskCreate(ota_task, "nb_ota", 8192, arg, 5, NULL) != pdPASS) {
        free(arg);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"status\":\"started\"}");
}

/* ── Persona Export / Import (Etapa 15.2) ───────────────────────────────── */

static esp_err_t handle_api_persona_export(httpd_req_t *req)
{
    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"warmth\":%.4f,\"energy\":%.4f,\"curiosity\":%.4f,\"trust\":%.4f,"
        "\"total_touch\":%lu,\"sessions\":%lu,\"hours_alive\":%lu,\"familiar\":%s}",
        (double)persona_get_warmth(), (double)persona_get_energy(),
        (double)persona_get_curiosity(), (double)persona_get_trust(),
        (unsigned long)ltm_get_total_touch_count(),
        (unsigned long)ltm_get_total_sessions(),
        (unsigned long)ltm_get_hours_alive(),
        ltm_is_user_familiar() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"noisebot_persona.json\"");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_persona_import(httpd_req_t *req)
{
    char body[MAX_BODY_LEN];
    if (!recv_body(req, body, sizeof(body), NULL)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_ParseWithLength(body, strlen(body));
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON"); return ESP_OK; }

    const cJSON *w = cJSON_GetObjectItemCaseSensitive(root, "warmth");
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(root, "energy");
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "curiosity");
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "trust");

    if (!cJSON_IsNumber(w) || !cJSON_IsNumber(e) ||
        !cJSON_IsNumber(c) || !cJSON_IsNumber(t)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing dimensions");
        return ESP_OK;
    }

#define CLAMP01(x) ((x) < 0.0f ? 0.0f : ((x) > 1.0f ? 1.0f : (x)))
    float warmth    = CLAMP01((float)w->valuedouble);
    float energy    = CLAMP01((float)e->valuedouble);
    float curiosity = CLAMP01((float)c->valuedouble);
    float trust     = CLAMP01((float)t->valuedouble);
#undef CLAMP01
    cJSON_Delete(root);

    nvs_handle_t h;
    if (nvs_hal_open("nb_persona", NVS_READWRITE, &h) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"nvs failed\"}");
    }

    uint32_t u;
    memcpy(&u, &warmth,    sizeof(u)); nvs_hal_set_u32(h, "p_warmth",    u);
    memcpy(&u, &energy,    sizeof(u)); nvs_hal_set_u32(h, "p_energy",    u);
    memcpy(&u, &curiosity, sizeof(u)); nvs_hal_set_u32(h, "p_curiosity", u);
    memcpy(&u, &trust,     sizeof(u)); nvs_hal_set_u32(h, "p_trust",     u);
    nvs_hal_commit(h);
    nvs_hal_close(h);

    NB_LOGI(TAG, "persona importada: w=%.3f e=%.3f c=%.3f t=%.3f",
            (double)warmth, (double)energy, (double)curiosity, (double)trust);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req,
        "{\"ok\":true,\"note\":\"effective after restart\"}");
}

/* ── Etapa 15.3 — Sistema e diagnóstico ──────────────────────────────────── */

static esp_err_t handle_api_version(httpd_req_t *req)
{
    const esp_app_desc_t *d = esp_app_get_description();
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"version\":\"%.31s\",\"project\":\"%.31s\","
        "\"idf_ver\":\"%.31s\",\"build_date\":\"%.15s\",\"build_time\":\"%.15s\"}",
        d->version, d->project_name, d->idf_ver, d->date, d->time);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_health(httpd_req_t *req)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"heap_dram_free\":%lu,\"heap_dram_min\":%lu,"
        "\"heap_psram_free\":%lu,\"heap_psram_min\":%lu,"
        "\"task_count\":%lu,\"uptime_s\":%lu,\"health\":%u}",
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
        (unsigned long)uxTaskGetNumberOfTasks(),
        (unsigned long)diagnostics_get_uptime_s(),
        (unsigned)diagnostics_get_health_score());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_restart(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restarting\":true}");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handle_api_logs(httpd_req_t *req)
{
    taskENTER_CRITICAL(&s_log_mux);
    uint32_t count = s_log_count;
    uint32_t head  = s_log_head;
    taskEXIT_CRITICAL(&s_log_mux);

    uint32_t start = (count < LOG_RING_SIZE)
                     ? 0
                     : head;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    char esc[LOG_LINE_MAX * 2];
    char item[LOG_LINE_MAX * 2 + 8];
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (start + i) % LOG_RING_SIZE;
        json_escape(s_log_ring[idx], esc, sizeof(esc));
        snprintf(item, sizeof(item), "%s\"%s\"", i > 0 ? "," : "", esc);
        httpd_resp_sendstr_chunk(req, item);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── Etapa 15.4 — Controle expandido ────────────────────────────────────── */

static esp_err_t handle_api_expression(httpd_req_t *req)
{
    char body[MAX_BODY_LEN];
    if (!recv_body(req, body, sizeof(body), NULL)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_ParseWithLength(body, strlen(body));
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON"); return ESP_OK; }

    const cJSON *expr_j     = cJSON_GetObjectItemCaseSensitive(root, "expression");
    const cJSON *transit_j  = cJSON_GetObjectItemCaseSensitive(root, "transition_ms");
    float transit_ms = cJSON_IsNumber(transit_j) ? (float)transit_j->valuedouble : 300.0f;

    httpd_resp_set_type(req, "application/json");
    if (cJSON_IsString(expr_j)) {
        nb_expression_t expr = expr_from_str(expr_j->valuestring);
        expression_service_set(expr, transit_ms);
        NB_LOGI(TAG, "expression forçada via API: %s", expr_j->valuestring);
        cJSON_Delete(root);
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    cJSON_Delete(root);
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing expression\"}");
}

static esp_err_t handle_api_emot_event(httpd_req_t *req)
{
    char body[MAX_BODY_LEN];
    if (!recv_body(req, body, sizeof(body), NULL)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_ParseWithLength(body, strlen(body));
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON"); return ESP_OK; }

    const cJSON *ev_j = cJSON_GetObjectItemCaseSensitive(root, "event");
    httpd_resp_set_type(req, "application/json");
    if (cJSON_IsString(ev_j)) {
        const char *ev = ev_j->valuestring;
        for (int i = 0; i < NB_EMOT_EVT_COUNT; i++) {
            if (strcmp(ev, k_emot_evt_names[i]) == 0) {
                emotion_model_on_event((nb_emotion_event_t)i);
                NB_LOGI(TAG, "emot_event via API: %s", ev);
                cJSON_Delete(root);
                return httpd_resp_sendstr(req, "{\"ok\":true}");
            }
        }
    }
    cJSON_Delete(root);
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unknown event\"}");
}

static esp_err_t handle_api_emotion_get(httpd_req_t *req)
{
    nb_emotion_vec_t vec  = emotion_model_get_vec();
    nb_expression_t  expr = emotion_model_get_expression();
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"valence\":%.3f,\"activation\":%.3f,\"expression\":\"%s\"}",
        (double)vec.valence, (double)vec.activation,
        (unsigned)expr < NB_EXPR_COUNT ? k_expr_names[(unsigned)expr] : "UNKNOWN");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_led_post(httpd_req_t *req)
{
    char body[MAX_BODY_LEN];
    if (!recv_body(req, body, sizeof(body), NULL)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_ParseWithLength(body, strlen(body));
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON"); return ESP_OK; }

    const cJSON *r_j   = cJSON_GetObjectItemCaseSensitive(root, "r");
    const cJSON *g_j   = cJSON_GetObjectItemCaseSensitive(root, "g");
    const cJSON *b_j   = cJSON_GetObjectItemCaseSensitive(root, "b");
    const cJSON *idx_j = cJSON_GetObjectItemCaseSensitive(root, "idx");
    const cJSON *all_j = cJSON_GetObjectItemCaseSensitive(root, "all");

    if (cJSON_IsNumber(r_j) && cJSON_IsNumber(g_j) && cJSON_IsNumber(b_j)) {
        nb_led_color_t color = {
            .r = (uint8_t)r_j->valuedouble,
            .g = (uint8_t)g_j->valuedouble,
            .b = (uint8_t)b_j->valuedouble,
        };
        if (cJSON_IsTrue(all_j) || !cJSON_IsNumber(idx_j)) {
            NB_LOGI(TAG, "api LED all=%u,%u,%u",
                    (unsigned)color.r, (unsigned)color.g, (unsigned)color.b);
            led_set_all(color);
        } else {
            uint8_t idx = (uint8_t)idx_j->valuedouble;
            NB_LOGI(TAG, "api LED[%u]=%u,%u,%u",
                    (unsigned)idx, (unsigned)color.r,
                    (unsigned)color.g, (unsigned)color.b);
            led_set_color(idx, color);
        }
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }

    const cJSON *brightness_j = cJSON_GetObjectItemCaseSensitive(root, "brightness");
    if (cJSON_IsNumber(brightness_j)) {
        uint8_t brightness = (uint8_t)brightness_j->valuedouble;
        NB_LOGI(TAG, "api LED brightness=%u", (unsigned)brightness);
        led_set_brightness(brightness);
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }

    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing r/g/b or brightness");
    return ESP_OK;
}

static esp_err_t handle_api_gaze_get(httpd_req_t *req)
{
    float x = 0.0f, y = 0.0f;
    gaze_service_get_current(&x, &y);
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"x\":%.3f,\"y\":%.3f}", (double)x, (double)y);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_gaze_post(httpd_req_t *req)
{
    char body[MAX_BODY_LEN];
    if (!recv_body(req, body, sizeof(body), NULL)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_ParseWithLength(body, strlen(body));
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON"); return ESP_OK; }

    const cJSON *x_j = cJSON_GetObjectItemCaseSensitive(root, "x");
    const cJSON *y_j = cJSON_GetObjectItemCaseSensitive(root, "y");

    httpd_resp_set_type(req, "application/json");
    if (cJSON_IsNumber(x_j) && cJSON_IsNumber(y_j)) {
        float x = (float)x_j->valuedouble;
        float y = (float)y_j->valuedouble;
        if (x < -1.0f) { x = -1.0f; } else if (x > 1.0f) { x = 1.0f; }
        if (y < -1.0f) { y = -1.0f; } else if (y > 1.0f) { y = 1.0f; }
        gaze_service_set_target(x, y);
        cJSON_Delete(root);
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    cJSON_Delete(root);
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing x/y\"}");
}

static esp_err_t handle_api_circadian(httpd_req_t *req)
{
    nb_circadian_phase_t ph = circadian_get_phase();
    char buf[64];
    snprintf(buf, sizeof(buf),
        "{\"phase\":\"%s\",\"uptime_s\":%lu}",
        k_circadian_names[(int)ph < 3 ? (int)ph : 1],
        (unsigned long)diagnostics_get_uptime_s());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_audio(httpd_req_t *req)
{
    char buf[192];
    snprintf(buf, sizeof(buf),
        "{\"volume\":%u,\"bpm\":%.1f,\"bpm_conf\":%.2f,"
        "\"rms\":%.3f,\"dominant_freq\":%.1f}",
        (unsigned)config_get_volume(),
        (double)rhythm_service_get_bpm(),
        (double)rhythm_service_get_confidence(),
        (double)sound_analysis_get_rms(),
        (double)sound_analysis_get_dominant_freq());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_idle_post(httpd_req_t *req)
{
    char body[MAX_BODY_LEN];
    if (!recv_body(req, body, sizeof(body), NULL)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_ParseWithLength(body, strlen(body));
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON"); return ESP_OK; }

    const cJSON *sacc_j = cJSON_GetObjectItemCaseSensitive(root, "saccade_mult");
    const cJSON *yawn_j = cJSON_GetObjectItemCaseSensitive(root, "yawn_mult");
    if (cJSON_IsNumber(sacc_j)) idle_service_set_saccade_multiplier((float)sacc_j->valuedouble);
    if (cJSON_IsNumber(yawn_j)) idle_service_set_yawn_multiplier((float)yawn_j->valuedouble);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ── Etapa 15.5 — WiFi provisioning e config completa ───────────────────── */

static esp_err_t handle_api_wifi_get(httpd_req_t *req)
{
    bool connected  = wifi_service_is_connected();
    const char *ip  = wifi_service_get_ip();
    const char *sid = wifi_service_get_ssid();
    int8_t rssi     = wifi_service_get_rssi();
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d}",
        connected ? "true" : "false", sid, ip, (int)rssi);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_wifi_post(httpd_req_t *req)
{
    char body[MAX_BODY_LEN];
    if (!recv_body(req, body, sizeof(body), NULL)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    cJSON *root = cJSON_ParseWithLength(body, strlen(body));
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON"); return ESP_OK; }

    const cJSON *ssid_j = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *pass_j = cJSON_GetObjectItemCaseSensitive(root, "pass");

    httpd_resp_set_type(req, "application/json");
    if (cJSON_IsString(ssid_j)) {
        const char *pass = cJSON_IsString(pass_j) ? pass_j->valuestring : "";
        esp_err_t err = wifi_service_set_credentials(ssid_j->valuestring, pass);
        cJSON_Delete(root);
        if (err == ESP_OK)
            return httpd_resp_sendstr(req, "{\"ok\":true}");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"nvs failed\"}");
    }
    cJSON_Delete(root);
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing ssid\"}");
}

static esp_err_t handle_api_wifi_delete(httpd_req_t *req)
{
    wifi_service_clear_credentials();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_api_config_all(httpd_req_t *req)
{
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"volume\":%u,\"brightness\":%u,\"touch_sens\":%u,"
        "\"idle_timeout\":%lu,"
        "\"srv1_min\":%d,\"srv1_max\":%d,\"srv1_ctr\":%d,"
        "\"srv2_min\":%d,\"srv2_max\":%d,\"srv2_ctr\":%d,"
        "\"last_emotion\":%u,\"persona_seed\":%lu}",
        (unsigned)config_get_volume(),
        (unsigned)config_get_brightness(),
        (unsigned)config_get_touch_sensitivity(),
        (unsigned long)config_get_idle_timeout_s(),
        (int)config_get_servo_limit_min(1),
        (int)config_get_servo_limit_max(1),
        (int)config_get_servo_center(1),
        (int)config_get_servo_limit_min(2),
        (int)config_get_servo_limit_max(2),
        (int)config_get_servo_center(2),
        (unsigned)config_get_last_emotion(),
        (unsigned long)config_get_persona_seed());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/* ── Etapa 15.6 — LTM e persona avançada ───────────────────────────────── */

static esp_err_t handle_api_ltm_get(httpd_req_t *req)
{
    char buf[192];
    snprintf(buf, sizeof(buf),
        "{\"total_touch\":%lu,\"sessions\":%lu,"
        "\"hours_alive\":%lu,\"familiar\":%s,"
        "\"voice_count\":%lu,\"audio_played\":%lu}",
        (unsigned long)ltm_get_total_touch_count(),
        (unsigned long)ltm_get_total_sessions(),
        (unsigned long)ltm_get_hours_alive(),
        ltm_is_user_familiar() ? "true" : "false",
        (unsigned long)ltm_count_iact(LTM_IACT_VOICE_START),
        (unsigned long)ltm_count_iact(LTM_IACT_AUDIO_PLAYED));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_persona_delete(httpd_req_t *req)
{
    /* Reset das dimensões de persona para 0.5 (neutro) em NVS. */
    nvs_handle_t h;
    if (nvs_hal_open("nb_persona", NVS_READWRITE, &h) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"nvs failed\"}");
    }
    float mid = 0.5f;
    uint32_t u;
    memcpy(&u, &mid, sizeof(u));
    nvs_hal_set_u32(h, "p_warmth",    u);
    nvs_hal_set_u32(h, "p_energy",    u);
    nvs_hal_set_u32(h, "p_curiosity", u);
    nvs_hal_set_u32(h, "p_trust",     u);
    nvs_hal_commit(h);
    nvs_hal_close(h);
    NB_LOGI(TAG, "persona resetada para 0.5 via API");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req,
        "{\"ok\":true,\"note\":\"effective after restart\"}");
}

static esp_err_t handle_api_touch_get(httpd_req_t *req)
{
    nb_touch_debug_t dbg;
    touch_service_get_debug(&dbg);

    nb_event_type_t last_event;
    int64_t last_us;
    taskENTER_CRITICAL(&s_mux);
    last_event = s_last_touch_event;
    last_us    = s_last_touch_us;
    taskEXIT_CRITICAL(&s_mux);

    int64_t now_us = esp_timer_get_time();
    int64_t age_ms = (last_us > 0) ? ((now_us - last_us) / 1000) : -1;
    bool pressed = dbg.state != NB_TOUCH_STATE_IDLE;

    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"raw\":%lu,\"filtered\":%lu,\"baseline\":%lu,"
        "\"threshold_on\":%lu,\"threshold_off\":%lu,"
        "\"state\":\"%s\",\"pressed\":%s,\"duration_ms\":%lu,"
        "\"last_event\":\"%s\",\"last_event_age_ms\":%lld}",
        (unsigned long)dbg.raw,
        (unsigned long)dbg.filtered_raw,
        (unsigned long)dbg.baseline,
        (unsigned long)dbg.threshold_on,
        (unsigned long)dbg.threshold_off,
        touch_state_name(dbg.state),
        pressed ? "true" : "false",
        (unsigned long)dbg.touch_duration_ms,
        touch_event_name(last_event),
        (long long)age_ms);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/* ── Registro de handlers ────────────────────────────────────────────────── */

/* ── Etapa 12.23 — Diagnóstico de Produto ────────────────────────────────── */

static const char *transport_name(nb_bridge_transport_t t)
{
    switch (t) {
        case NB_BRIDGE_TRANSPORT_TCP:  return "tcp";
        case NB_BRIDGE_TRANSPORT_UART: return "uart";
        default:                       return "offline";
    }
}

static esp_err_t handle_api_diag(httpd_req_t *req)
{
    const esp_app_desc_t *d  = esp_app_get_description();
    nb_bridge_transport_t bt = bridge_service_get_transport();
    uint32_t rx_age          = bridge_service_get_last_rx_age_ms();
    char rx_buf[16];
    if (rx_age == UINT32_MAX) {
        snprintf(rx_buf, sizeof(rx_buf), "null");
    } else {
        snprintf(rx_buf, sizeof(rx_buf), "%lu", (unsigned long)rx_age);
    }

    char buf[640];
    snprintf(buf, sizeof(buf),
        "{"
        "\"version\":\"%.31s\","
        "\"state\":\"%s\","
        "\"bridge\":{\"connected\":%s,\"transport\":\"%s\","
                    "\"protocol_v\":%u,\"last_rx_ms\":%s},"
        "\"wake\":{\"active\":%s,\"model\":\"WakeNet9/Hi ESP\","
                  "\"threshold\":%.2f,\"detections\":%lu},"
        "\"audio\":{\"rms\":%.4f,\"listening\":%s},"
        "\"memory\":{\"psram_free\":%lu,\"dram_free\":%lu},"
        "\"fps\":%.1f,"
        "\"health\":%u,"
        "\"uptime_s\":%lu,"
        "\"touch_count\":%lu,"
        "\"sessions\":%lu,"
        "\"hours_alive\":%lu"
        "}",
        d->version,
        state_machine_state_name(state_machine_get_state()),
        bridge_service_is_connected() ? "true" : "false",
        transport_name(bt),
        (unsigned)bridge_service_get_protocol_version(),
        rx_buf,
        wake_service_is_active() ? "true" : "false",
        (double)wake_service_get_threshold(),
        (unsigned long)wake_service_get_detect_count(),
        (double)sound_analysis_get_rms(),
        audio_service_is_listening() ? "true" : "false",
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned long)esp_get_free_heap_size(),
        (double)diagnostics_get_fps(),
        (unsigned)diagnostics_get_health_score(),
        (unsigned long)diagnostics_get_uptime_s(),
        (unsigned long)ltm_get_total_touch_count(),
        (unsigned long)ltm_get_total_sessions(),
        (unsigned long)ltm_get_hours_alive());

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_diag_snapshot(httpd_req_t *req)
{
    diagnostics_dump_to_sd();

    const esp_app_desc_t *d = esp_app_get_description();
    uint8_t vol = config_get_volume();

    /* Coleta últimas 5 linhas do log ring como erros recentes. */
    taskENTER_CRITICAL(&s_log_mux);
    uint32_t count = s_log_count;
    uint32_t head  = s_log_head;
    taskEXIT_CRITICAL(&s_log_mux);

    uint32_t n = (count < 5u) ? count : 5u;
    uint32_t start = (count < LOG_RING_SIZE)
                     ? ((count > n) ? count - n : 0u)
                     : ((head + LOG_RING_SIZE - n) % LOG_RING_SIZE);

    char buf[768];
    int pos = snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"version\":\"%.31s\","
        "\"health\":%u,\"uptime_s\":%lu,"
        "\"config\":{\"volume\":%u},"
        "\"recent_logs\":[",
        d->version,
        (unsigned)diagnostics_get_health_score(),
        (unsigned long)diagnostics_get_uptime_s(),
        (unsigned)vol);

    for (uint32_t i = 0; i < n && pos < (int)sizeof(buf) - 8; i++) {
        uint32_t idx = (start + i) % LOG_RING_SIZE;
        char line[LOG_LINE_MAX + 4];
        /* Escapa aspas simples para JSON seguro. */
        const char *src = s_log_ring[idx];
        int lp = 0;
        line[lp++] = '"';
        for (int j = 0; src[j] && lp < (int)sizeof(line) - 3; j++) {
            if (src[j] == '"' || src[j] == '\\') line[lp++] = '\\';
            line[lp++] = src[j];
        }
        line[lp++] = '"';
        line[lp]   = '\0';
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                        "%s%s", (i > 0 ? "," : ""), line);
    }
    if (pos < (int)sizeof(buf) - 4) {
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "]}");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_diag_wake(httpd_req_t *req)
{
    char buf[192];
    snprintf(buf, sizeof(buf),
        "{\"active\":%s,\"model\":\"WakeNet9\",\"keyword\":\"Hi ESP\","
        "\"threshold\":%.2f,\"detections\":%lu}",
        wake_service_is_active() ? "true" : "false",
        (double)wake_service_get_threshold(),
        (unsigned long)wake_service_get_detect_count());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_diag_bridge(httpd_req_t *req)
{
    nb_bridge_transport_t bt = bridge_service_get_transport();
    uint32_t rx_age          = bridge_service_get_last_rx_age_ms();
    char rx_buf[16];
    if (rx_age == UINT32_MAX) {
        snprintf(rx_buf, sizeof(rx_buf), "null");
    } else {
        snprintf(rx_buf, sizeof(rx_buf), "%lu", (unsigned long)rx_age);
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"connected\":%s,\"transport\":\"%s\","
        "\"protocol_v\":%u,\"last_rx_ms\":%s,"
        "\"port\":%d}",
        bridge_service_is_connected() ? "true" : "false",
        transport_name(bt),
        (unsigned)bridge_service_get_protocol_version(),
        rx_buf,
        NB_BRIDGE_TCP_PORT);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static const httpd_uri_t k_uris[] = {
    { .uri = "/",            .method = HTTP_GET,  .handler = handle_root },
    { .uri = "/app.js",      .method = HTTP_GET,  .handler = handle_app_js },
    { .uri = "/style.css",   .method = HTTP_GET,  .handler = handle_style_css },
    { .uri = "/api/status",  .method = HTTP_GET,  .handler = handle_api_status },
    { .uri = "/api/persona", .method = HTTP_GET,  .handler = handle_api_persona },
    { .uri = "/api/config",  .method = HTTP_GET,  .handler = handle_api_config_get },
    { .uri = "/api/config",  .method = HTTP_POST, .handler = handle_api_config_post },
    { .uri = "/api/command",        .method = HTTP_POST,   .handler = handle_api_command },
    { .uri = "/api/servo",          .method = HTTP_POST,   .handler = handle_api_servo_post },
    { .uri = "/api/ota",            .method = HTTP_POST,   .handler = handle_api_ota },
    { .uri = "/api/persona/export", .method = HTTP_GET,    .handler = handle_api_persona_export },
    { .uri = "/api/persona/import", .method = HTTP_POST,   .handler = handle_api_persona_import },
    /* 15.3 — Sistema */
    { .uri = "/api/version",        .method = HTTP_GET,    .handler = handle_api_version },
    { .uri = "/api/health",         .method = HTTP_GET,    .handler = handle_api_health },
    { .uri = "/api/restart",        .method = HTTP_POST,   .handler = handle_api_restart },
    { .uri = "/api/logs",           .method = HTTP_GET,    .handler = handle_api_logs },
    /* 15.4 — Controle expandido */
    { .uri = "/api/expression",     .method = HTTP_POST,   .handler = handle_api_expression },
    { .uri = "/api/emot_event",     .method = HTTP_POST,   .handler = handle_api_emot_event },
    { .uri = "/api/emotion",        .method = HTTP_GET,    .handler = handle_api_emotion_get },
    { .uri = "/api/led",            .method = HTTP_POST,   .handler = handle_api_led_post },
    { .uri = "/api/gaze",           .method = HTTP_GET,    .handler = handle_api_gaze_get },
    { .uri = "/api/gaze",           .method = HTTP_POST,   .handler = handle_api_gaze_post },
    { .uri = "/api/circadian",      .method = HTTP_GET,    .handler = handle_api_circadian },
    { .uri = "/api/audio",          .method = HTTP_GET,    .handler = handle_api_audio },
    { .uri = "/api/idle",           .method = HTTP_POST,   .handler = handle_api_idle_post },
    /* 15.5 — WiFi */
    { .uri = "/api/wifi",           .method = HTTP_GET,    .handler = handle_api_wifi_get },
    { .uri = "/api/wifi",           .method = HTTP_POST,   .handler = handle_api_wifi_post },
    { .uri = "/api/wifi",           .method = HTTP_DELETE, .handler = handle_api_wifi_delete },
    /* 15.5 — Config expandida */
    { .uri = "/api/config/all",     .method = HTTP_GET,    .handler = handle_api_config_all },
    /* 15.6 — LTM e persona */
    { .uri = "/api/ltm",            .method = HTTP_GET,    .handler = handle_api_ltm_get },
    { .uri = "/api/persona",        .method = HTTP_DELETE, .handler = handle_api_persona_delete },
    { .uri = "/api/touch",          .method = HTTP_GET,    .handler = handle_api_touch_get },
    /* 12.23 — Diagnóstico de produto */
    { .uri = "/api/diag",               .method = HTTP_GET,  .handler = handle_api_diag },
    { .uri = "/api/diag/snapshot",      .method = HTTP_POST, .handler = handle_api_diag_snapshot },
    { .uri = "/api/diag/test/wake",     .method = HTTP_GET,  .handler = handle_api_diag_wake },
    { .uri = "/api/diag/test/bridge",   .method = HTTP_GET,  .handler = handle_api_diag_bridge },
    {
        .uri          = "/ws",
        .method       = HTTP_GET,
        .handler      = handle_ws,
        .is_websocket = true,
    },
};

/* ── Início do servidor ──────────────────────────────────────────────────── */

static void web_service_start(void)
{
    if (s_server) return;

    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets  = 4;
    cfg.max_uri_handlers  = 40;   /* 30 handlers registrados + margem */
    cfg.server_port       = 80;
    cfg.lru_purge_enable  = true;
    cfg.uri_match_fn      = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        NB_LOGE(TAG, "httpd_start falhou: %s", esp_err_to_name(err));
        s_server = NULL;
        return;
    }

    for (size_t i = 0; i < sizeof(k_uris) / sizeof(k_uris[0]); i++) {
        httpd_register_uri_handler(s_server, &k_uris[i]);
    }

    /* Timer de telemetria a cada 2s — push audio/emotion/circadian via WS. */
    if (!s_audio_tmr) {
        const esp_timer_create_args_t ta = {
            .callback = audio_push_timer_cb,
            .name     = "web_telemetry",
        };
        esp_timer_create(&ta, &s_audio_tmr);
        esp_timer_start_periodic(s_audio_tmr, 2000000LL); /* 2s */
    }

    NB_LOGI(TAG, "HTTP server iniciado na porta 80 — http://noisebot.local");
}

/* ── Handlers de eventos ─────────────────────────────────────────────────── */

static void on_ip_acquired(const nb_event_t *ev, void *ctx)
{
    (void)ev; (void)ctx;
    web_service_start();
}

static void on_state_changed(const nb_event_t *ev, void *ctx)
{
    (void)ev; (void)ctx;
    ws_push_status();
}

static void on_touch_debug_event(const nb_event_t *ev, void *ctx)
{
    (void)ctx;
    taskENTER_CRITICAL(&s_mux);
    s_last_touch_event = ev->type;
    s_last_touch_us    = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_mux);
}

/* ── API pública ─────────────────────────────────────────────────────────── */

esp_err_t web_service_init(void)
{
    /* Timer de auto-resume do conductor após calibração de servo */
    if (!s_servo_calib_tmr) {
        const esp_timer_create_args_t ta = {
            .callback = servo_calib_resume_cb,
            .name     = "srv_calib",
        };
        esp_timer_create(&ta, &s_servo_calib_tmr);
    }

    /* Intercepta logs do ESP-IDF para ring buffer em RAM. */
    s_orig_vprintf = esp_log_set_vprintf(log_hook_vprintf);

    nb_event_subscribe(NB_EVT_PERSONA_REFRESHED, on_persona_refreshed, NULL, NULL);
    nb_event_subscribe(NB_EVT_WIFI_IP_ACQUIRED, on_ip_acquired,  NULL, NULL);
    nb_event_subscribe(NB_EVT_STATE_CHANGED,    on_state_changed, NULL, NULL);
    nb_event_subscribe(NB_EVT_TOUCH_TAP,         on_touch_debug_event, NULL, NULL);
    nb_event_subscribe(NB_EVT_TOUCH_LONG_PRESS,  on_touch_debug_event, NULL, NULL);
    nb_event_subscribe(NB_EVT_TOUCH_SUSTAINED,   on_touch_debug_event, NULL, NULL);
    nb_event_subscribe(NB_EVT_TOUCH_WAKE,        on_touch_debug_event, NULL, NULL);
    nb_event_subscribe(NB_EVT_TOUCH_DOUBLE_TAP,  on_touch_debug_event, NULL, NULL);
    nb_event_subscribe(NB_EVT_TOUCH_DEEP,        on_touch_debug_event, NULL, NULL);
    nb_event_subscribe(NB_EVT_TOUCH_CARESS,      on_touch_debug_event, NULL, NULL);
    nb_event_subscribe(NB_EVT_TOUCH_WARM_PULSE,  on_touch_debug_event, NULL, NULL);
    NB_LOGI(TAG, "web_service registrado — aguardando IP");
    return ESP_OK;
}
