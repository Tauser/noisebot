/*
 * web_service.c — Companion API minima (Layer 2, Etapas 15.1–15.6)
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
#include "audio_processor_service.h"
#include "audio_io_service_v2.h"
#include "audio_playback_service_v2.h"
#include "voice_capture_session_v2.h"
#include "audio_service.h"
#include "touch_service.h"
#include "time_service.h"
#include "agenda_service.h"
#include "camera_service.h"
#include "vision_service.h"
#include "synth_service.h"
#include "sd_hal.h"
#include "nb_hw_config.h"
#include <dirent.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define TAG              "nb_web"
#define MAX_BODY_LEN     512
#define HTTPD_TASK_STACK_SIZE 6144

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
static portMUX_TYPE       s_mux        = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_http_health_tmr = NULL;
static esp_timer_handle_t s_servo_calib_tmr = NULL;  /* auto-resume conductor após calibração */
static nb_event_type_t    s_last_touch_event = NB_EVT_NONE;
static int64_t            s_last_touch_us    = 0;
static volatile bool      s_http_restart_pending = false;

typedef struct {
    char     provider[24];
    char     model[40];
    char     route[24];
    char     outcome[32];
    char     detail[40];
    uint32_t session_id;
    uint32_t stt_ms;
    uint32_t llm_ms;
    uint32_t tts_ms;
    int64_t  updated_us;
} nb_ai_status_state_t;

static nb_ai_status_state_t s_ai_state = {
    .provider = "none",
    .model    = "none",
    .route    = "none",
    .outcome  = "unknown",
    .detail   = "none",
};

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

static const char *capture_v2_state_name(nb_voice_capture_v2_state_t state)
{
    switch (state) {
        case NB_VOICE_CAPTURE_V2_IDLE_SESSION:       return "IDLE_SESSION";
        case NB_VOICE_CAPTURE_V2_WAITING_FOR_SPEECH: return "WAITING_FOR_SPEECH";
        case NB_VOICE_CAPTURE_V2_CAPTURING:          return "CAPTURING";
        case NB_VOICE_CAPTURE_V2_ENDING_ON_SILENCE:  return "ENDING_ON_SILENCE";
        case NB_VOICE_CAPTURE_V2_CANCELLED:          return "CANCELLED";
        case NB_VOICE_CAPTURE_V2_DONE:               return "DONE";
        default:                                     return "UNKNOWN";
    }
}

static const char *capture_v2_source_name(nb_voice_capture_v2_source_t source)
{
    switch (source) {
        case NB_VOICE_CAPTURE_V2_SOURCE_WAKE_WORD: return "WAKE_WORD";
        case NB_VOICE_CAPTURE_V2_SOURCE_BARGE_IN:  return "BARGE_IN";
        case NB_VOICE_CAPTURE_V2_SOURCE_FOLLOWUP:  return "FOLLOWUP";
        case NB_VOICE_CAPTURE_V2_SOURCE_DEBUG:     return "DEBUG";
        default:                                   return "UNKNOWN";
    }
}

static nb_voice_capture_v2_source_t capture_v2_source_from_str(const char *value)
{
    if (value == NULL) {
        return NB_VOICE_CAPTURE_V2_SOURCE_DEBUG;
    }
    if (strcmp(value, "wake") == 0 || strcmp(value, "WAKE_WORD") == 0) {
        return NB_VOICE_CAPTURE_V2_SOURCE_WAKE_WORD;
    }
    if (strcmp(value, "barge") == 0 || strcmp(value, "BARGE_IN") == 0) {
        return NB_VOICE_CAPTURE_V2_SOURCE_BARGE_IN;
    }
    if (strcmp(value, "followup") == 0 || strcmp(value, "FOLLOWUP") == 0) {
        return NB_VOICE_CAPTURE_V2_SOURCE_FOLLOWUP;
    }
    return NB_VOICE_CAPTURE_V2_SOURCE_DEBUG;
}

static void copy_json_string(cJSON *root, const char *key, char *dst, size_t dst_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || !item->valuestring || dst_len == 0u) return;
    snprintf(dst, dst_len, "%s", item->valuestring);
}

static uint32_t get_json_u32(cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0.0) return 0u;
    return (uint32_t)item->valuedouble;
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

static void ota_progress_note(int pct, const char *status, const char *msg)
{
    (void)pct;
    (void)status;
    (void)msg;
    /* UI/WS removidos do firmware: progresso de OTA fica apenas no log. */
}

/* ── Handlers HTTP ───────────────────────────────────────────────────────── */

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

static bool recv_body(httpd_req_t *req, char *buf, size_t size, int *out_len);

static esp_err_t handle_api_camera_snapshot(httpd_req_t *req)
{
    if (!camera_service_is_supported()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"camera_disabled\"}");
    }
    if (audio_service_is_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"audio_busy\"}");
    }

    nb_camera_snapshot_t snap;
    esp_err_t err = camera_service_capture_snapshot(&snap);
    if (err != ESP_OK) {
        char buf[96];
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, buf);
    }

    uint8_t *snapshot_copy = (uint8_t *)heap_caps_malloc(snap.len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snapshot_copy) {
        camera_service_release_snapshot();
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"snapshot_copy_no_mem\"}");
    }
    memcpy(snapshot_copy, snap.data, snap.len);

    char dim[16];
    snprintf(dim, sizeof(dim), "%u", (unsigned)snap.width);
    httpd_resp_set_hdr(req, "X-Camera-Width", dim);
    snprintf(dim, sizeof(dim), "%u", (unsigned)snap.height);
    httpd_resp_set_hdr(req, "X-Camera-Height", dim);
    size_t snapshot_len = snap.len;
    camera_service_release_snapshot();

    httpd_resp_set_type(req, "image/jpeg");
    err = httpd_resp_send(req, (const char *)snapshot_copy, (ssize_t)snapshot_len);
    heap_caps_free(snapshot_copy);
    return err;
}

static esp_err_t handle_api_camera_mode(httpd_req_t *req)
{
    char body[96];
    int len = 0;
    if (!recv_body(req, body, sizeof(body), &len)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength(body, (size_t)len);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_OK;
    }

    const cJSON *mode_j = cJSON_GetObjectItem(root, "mode");
    const char *mode_s = cJSON_IsString(mode_j) ? mode_j->valuestring : "";
    nb_camera_mode_t mode;
    if (strcmp(mode_s, "better") == 0 || strcmp(mode_s, "qvga") == 0) {
        mode = NB_CAMERA_MODE_BETTER_QVGA;
    } else if (strcmp(mode_s, "safe") == 0 || strcmp(mode_s, "qqvga") == 0) {
        mode = NB_CAMERA_MODE_SAFE_QQVGA;
    } else {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mode");
        return ESP_OK;
    }
    cJSON_Delete(root);

    esp_err_t err = camera_service_set_mode(mode);
    if (err != ESP_OK) {
        char buf[80];
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, buf);
    }

    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"mode\":\"%s\",\"width\":%u,\"height\":%u}",
             camera_hal_mode_name(mode),
             (unsigned)camera_hal_mode_width(mode),
             (unsigned)camera_hal_mode_height(mode));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_camera_session_close(httpd_req_t *req)
{
    esp_err_t err = camera_service_close_session();
    if (err != ESP_OK) {
        char buf[80];
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, buf);
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_api_camera_status(httpd_req_t *req)
{
    nb_camera_diag_status_t diag;
    camera_service_get_diag_status(&diag);
    const char *last_error = diag.has_last_error ? esp_err_to_name(diag.last_error) : "";
    bool camera_ready = camera_service_is_ready();
    bool blocked_by_memory = false;
    char chunk[256];

    httpd_resp_set_type(req, "application/json");

    snprintf(chunk, sizeof(chunk),
             "{\"supported\":%s,\"ready\":%s,\"active\":%s,"
             "\"bridge_connected\":%s,",
             camera_service_is_supported() ? "true" : "false",
             camera_ready ? "true" : "false",
             camera_ready ? "true" : "false",
             bridge_service_is_connected() ? "true" : "false");
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
             "\"blocked_by_bridge\":false,\"blocked_by_audio\":%s,"
             "\"blocked_by_memory\":%s,\"last_error\":%ld,",
             audio_service_is_busy() ? "true" : "false",
             blocked_by_memory ? "true" : "false",
             (long)(diag.has_last_error ? diag.last_error : ESP_OK));
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
             "\"last_error_name\":\"%s\",\"last_error_phase\":\"%s\","
             "\"mode\":\"%s\","
             "\"mode_width\":%lu,\"mode_height\":%lu,",
             last_error,
             diag.last_error_phase ? diag.last_error_phase : "",
             diag.mode_name,
             (unsigned long)diag.mode_width,
             (unsigned long)diag.mode_height);
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
             "\"last_jpeg_bytes\":%lu,\"last_capture_ms\":%lu,"
             "\"capture_count\":%lu,\"fail_count\":%lu,",
             (unsigned long)diag.last_jpeg_bytes,
             (unsigned long)diag.last_capture_ms,
             (unsigned long)diag.capture_count,
             (unsigned long)diag.fail_count);
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
             "\"last_dma_before\":%lu,\"last_dma_after_capture\":%lu,"
             "\"last_dma_after_release\":%lu,",
             (unsigned long)diag.last_dma_before,
             (unsigned long)diag.last_dma_after_capture,
             (unsigned long)diag.last_dma_after_release);
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
             "\"last_dma_largest_before\":%lu,"
             "\"last_dma_largest_after_capture\":%lu,"
             "\"last_dma_largest_after_release\":%lu,",
             (unsigned long)diag.last_dma_largest_before,
             (unsigned long)diag.last_dma_largest_after_capture,
             (unsigned long)diag.last_dma_largest_after_release);
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
             "\"last_internal_before\":%lu,"
             "\"last_internal_after_capture\":%lu,"
             "\"last_internal_after_release\":%lu,"
             "\"last_psram_before\":%lu,",
             (unsigned long)diag.last_internal_before,
             (unsigned long)diag.last_internal_after_capture,
             (unsigned long)diag.last_internal_after_release,
             (unsigned long)diag.last_psram_before);
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
             "\"last_psram_after_capture\":%lu,"
             "\"last_psram_after_release\":%lu,",
             (unsigned long)diag.last_psram_after_capture,
             (unsigned long)diag.last_psram_after_release);
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
             "\"format\":\"jpeg\",\"width\":%lu,\"height\":%lu,"
             "\"heap_dma_free\":%lu,\"heap_dma_largest\":%lu,",
             (unsigned long)diag.mode_width,
             (unsigned long)diag.mode_height,
             (unsigned long)diag.dma_free,
             (unsigned long)diag.dma_largest);
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
             "\"heap_internal_free\":%lu,\"heap_psram_free\":%lu,"
             "\"min_dma_before\":%lu,\"min_dma_largest\":%lu,",
             (unsigned long)diag.internal_free,
             (unsigned long)diag.psram_free,
             (unsigned long)diag.min_dma_before,
             (unsigned long)diag.min_dma_largest);
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
             "\"min_internal_before\":%lu}",
             (unsigned long)diag.min_internal_before);
    httpd_resp_sendstr_chunk(req, chunk);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static void vision_observation_json(const nb_vision_observation_t *obs,
                                    char *buf,
                                    size_t buf_len)
{
    if (!obs || !buf || buf_len == 0U) {
        return;
    }

    snprintf(buf, buf_len,
             "{\"valid\":%s,\"scene\":\"%s\",\"timestamp_ms\":%lu,"
             "\"width\":%lu,\"height\":%lu,\"jpeg_bytes\":%lu,"
             "\"capture_ms\":%lu,\"luma_avg\":%u,\"luma_min\":%u,"
             "\"luma_max\":%u,\"contrast\":%u,\"motion_score\":%u}",
             obs->valid ? "true" : "false",
             vision_service_scene_name(obs->scene),
             (unsigned long)obs->timestamp_ms,
             (unsigned long)obs->width,
             (unsigned long)obs->height,
             (unsigned long)obs->jpeg_bytes,
             (unsigned long)obs->capture_ms,
             (unsigned)obs->luma_avg,
             (unsigned)obs->luma_min,
             (unsigned)obs->luma_max,
             (unsigned)obs->contrast,
             (unsigned)obs->motion_score);
}

static esp_err_t handle_api_vision_observe(httpd_req_t *req)
{
    if (!vision_service_is_available()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"vision_unavailable\"}");
    }
    if (audio_service_is_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"audio_busy\"}");
    }

    nb_vision_observation_t obs;
    esp_err_t err = vision_service_observe(&obs);
    if (err != ESP_OK) {
        char err_buf[96];
        snprintf(err_buf, sizeof(err_buf), "{\"ok\":false,\"error\":\"%s\"}",
                 esp_err_to_name(err));
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, err_buf);
    }

    char obs_buf[384];
    vision_observation_json(&obs, obs_buf, sizeof(obs_buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"ok\":true,\"observation\":");
    httpd_resp_sendstr_chunk(req, obs_buf);
    httpd_resp_sendstr_chunk(req, "}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t handle_api_vision_status(httpd_req_t *req)
{
    nb_vision_observation_t obs;
    vision_service_get_last(&obs);
    char obs_buf[384];
    vision_observation_json(&obs, obs_buf, sizeof(obs_buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"available\":");
    httpd_resp_sendstr_chunk(req, vision_service_is_available() ? "true" : "false");
    httpd_resp_sendstr_chunk(req, ",\"observation\":");
    httpd_resp_sendstr_chunk(req, obs_buf);
    httpd_resp_sendstr_chunk(req, "}");
    return httpd_resp_sendstr_chunk(req, NULL);
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

    if (cJSON_IsString(type_j) &&
        strcmp(type_j->valuestring, "ALERT") == 0 &&
        cJSON_IsString(value_j) &&
        strcmp(value_j->valuestring, "SILENCE") == 0)
    {
        synth_stop();
        NB_LOGI(TAG, "alerta silenciado via API");
        executed = true;
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

/* ── OTA (Etapa 15.2) ────────────────────────────────────────────────────── */

typedef struct { char url[256]; } ota_task_arg_t;

static void ota_task(void *arg)
{
    ota_task_arg_t *a = (ota_task_arg_t *)arg;
    char url[256];
    strlcpy(url, a->url, sizeof(url));
    heap_caps_free(a);

    NB_LOGI(TAG, "OTA: iniciando de %s", url);
    led_base_set(NB_LED_BASE_SAFE_MODE, true);
    ota_progress_note(0, "started", NULL);

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        NB_LOGE(TAG, "OTA: nenhuma partição OTA disponível");
        ota_progress_note(0, "error", "no OTA partition");
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
        ota_progress_note(0, "error", "http open failed");
        if (client) esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    int content_len = (int)esp_http_client_fetch_headers(client);

    esp_ota_handle_t ota_handle;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle) != ESP_OK) {
        NB_LOGE(TAG, "OTA: ota_begin falhou");
        ota_progress_note(0, "error", "ota begin failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ota_progress_note(0, "error", "no memory");
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
        if (pct != last_pct) { ota_progress_note(pct, "downloading", NULL); last_pct = pct; }
    }

    heap_caps_free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (write_err != ESP_OK || total == 0) {
        esp_ota_abort(ota_handle);
        ota_progress_note(0, "error", "download failed");
        vTaskDelete(NULL);
        return;
    }

    if (esp_ota_end(ota_handle) != ESP_OK ||
        esp_ota_set_boot_partition(part) != ESP_OK) {
        NB_LOGE(TAG, "OTA: finalização falhou");
        ota_progress_note(0, "error", "finalization failed");
        vTaskDelete(NULL);
        return;
    }

    NB_LOGI(TAG, "OTA: OK (%d bytes) — reiniciando em 3s", total);
    ota_progress_note(100, "complete", NULL);
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

    ota_task_arg_t *arg = (ota_task_arg_t *)heap_caps_malloc(sizeof(ota_task_arg_t),
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!arg) {
        arg = (ota_task_arg_t *)heap_caps_malloc(sizeof(ota_task_arg_t),
                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (arg) strlcpy(arg->url, url_j->valuestring, sizeof(arg->url));
    cJSON_Delete(root);

    BaseType_t ota_ok = arg ? xTaskCreate(ota_task, "nb_ota", 8192, arg, 5, NULL) : pdFAIL;
    if (!arg || ota_ok != pdPASS) {
        heap_caps_free(arg);
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

static void send_health_object(httpd_req_t *req, bool wrap)
{
    bool sd_mounted = sd_hal_is_mounted();
    uint64_t sd_free_bytes = 0;
    bool sd_free_ok = sd_mounted
        && sd_hal_get_free_bytes(&sd_free_bytes) == ESP_OK;
    char sd_free_buf[24];
    if (sd_free_ok) {
        snprintf(sd_free_buf, sizeof(sd_free_buf), "%llu",
                 (unsigned long long)sd_free_bytes);
    } else {
        snprintf(sd_free_buf, sizeof(sd_free_buf), "null");
    }

    char buf[768];
    snprintf(buf, sizeof(buf),
        "%s\"heap_dram_free\":%lu,\"heap_dram_min\":%lu,"
        "\"heap_internal_free\":%lu,\"heap_internal_min\":%lu,"
        "\"heap_internal_largest\":%lu,"
        "\"heap_dma_free\":%lu,\"heap_dma_min\":%lu,\"heap_dma_largest\":%lu,"
        "\"heap_psram_free\":%lu,\"heap_psram_min\":%lu,"
        "\"heap_psram_largest\":%lu,"
        "\"storage\":{\"sd_mounted\":%s,\"sd_free_bytes\":%s},"
        "\"task_count\":%lu,\"uptime_s\":%lu,\"health\":%u%s",
        wrap ? "{" : "",
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA),
        (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
        sd_mounted ? "true" : "false",
        sd_free_buf,
        (unsigned long)uxTaskGetNumberOfTasks(),
        (unsigned long)diagnostics_get_uptime_s(),
        (unsigned)diagnostics_get_health_score(),
        wrap ? "}" : "");
    httpd_resp_sendstr_chunk(req, buf);
}

static esp_err_t handle_api_health(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    send_health_object(req, true);
    return httpd_resp_sendstr_chunk(req, NULL);
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

static esp_err_t send_audio_processor_status(httpd_req_t *req, esp_err_t probe_err)
{
    nb_audio_processor_status_t st;
    audio_processor_service_get_status(&st);

    char buf[1664];
    snprintf(buf, sizeof(buf),
             "{\"ok\":%s,\"initialized\":%s,\"enabled\":%s,"
             "\"probe_ran\":%s,\"probe_ok\":%s,"
             "\"aec_probe_ran\":%s,\"aec_probe_ok\":%s,"
             "\"aec_supported\":%s,"
             "\"aec_blocked_no_reference\":%s,"
             "\"shadow_active\":%s,\"shadow_stop_requested\":%s,"
             "\"processed_bridge_enabled\":%s,"
             "\"processed_capture_active\":%s,"
             "\"psram_before_kb\":%lu,"
             "\"psram_after_create_kb\":%lu,"
             "\"psram_after_destroy_kb\":%lu,"
             "\"aec_psram_before_kb\":%lu,"
             "\"aec_psram_after_create_kb\":%lu,"
             "\"aec_psram_after_destroy_kb\":%lu,"
             "\"internal_free_kb\":%lu,"
             "\"internal_largest_kb\":%lu,"
             "\"dma_free_kb\":%lu,"
             "\"dma_largest_kb\":%lu,"
             "\"shadow_psram_start_kb\":%lu,"
             "\"shadow_psram_current_kb\":%lu,"
             "\"shadow_feed_chunks\":%lu,"
             "\"shadow_fetch_chunks\":%lu,"
             "\"shadow_fetch_nulls\":%lu,"
             "\"shadow_feed_drops\":%lu,"
             "\"shadow_output_rms\":%lu,"
             "\"shadow_output_peak\":%u,"
             "\"processed_bridge_chunks\":%lu,"
             "\"processed_bridge_fallbacks\":%lu,"
             "\"processed_output_overruns\":%lu,"
             "\"processed_buffer_level\":%u,"
             "\"feed_chunksize\":%d,\"fetch_chunksize\":%d,"
             "\"feed_channels\":%d,\"fetch_channels\":%d,"
             "\"sample_rate_hz\":%d,"
             "\"last_error\":\"%s\",\"aec_last_error\":\"%s\","
             "\"probe_error\":\"%s\"}",
             (probe_err == ESP_OK) ? "true" : "false",
             st.initialized ? "true" : "false",
             st.enabled ? "true" : "false",
             st.probe_ran ? "true" : "false",
             st.probe_ok ? "true" : "false",
             st.aec_probe_ran ? "true" : "false",
             st.aec_probe_ok ? "true" : "false",
             st.aec_supported ? "true" : "false",
             st.aec_blocked_no_reference ? "true" : "false",
             st.shadow_active ? "true" : "false",
             st.shadow_stop_requested ? "true" : "false",
             st.processed_bridge_enabled ? "true" : "false",
             st.processed_capture_active ? "true" : "false",
             (unsigned long)st.psram_before_kb,
             (unsigned long)st.psram_after_create_kb,
             (unsigned long)st.psram_after_destroy_kb,
             (unsigned long)st.aec_psram_before_kb,
             (unsigned long)st.aec_psram_after_create_kb,
             (unsigned long)st.aec_psram_after_destroy_kb,
             (unsigned long)st.internal_free_kb,
             (unsigned long)st.internal_largest_kb,
             (unsigned long)st.dma_free_kb,
             (unsigned long)st.dma_largest_kb,
             (unsigned long)st.shadow_psram_start_kb,
             (unsigned long)st.shadow_psram_current_kb,
             (unsigned long)st.shadow_feed_chunks,
             (unsigned long)st.shadow_fetch_chunks,
             (unsigned long)st.shadow_fetch_nulls,
             (unsigned long)st.shadow_feed_drops,
             (unsigned long)st.shadow_output_rms,
             (unsigned)st.shadow_output_peak,
             (unsigned long)st.processed_bridge_chunks,
             (unsigned long)st.processed_bridge_fallbacks,
             (unsigned long)st.processed_output_overruns,
             (unsigned)st.processed_buffer_level,
             st.feed_chunksize,
             st.fetch_chunksize,
             st.feed_channels,
             st.fetch_channels,
             st.sample_rate_hz,
             esp_err_to_name(st.last_error),
             esp_err_to_name(st.aec_last_error),
             esp_err_to_name(probe_err));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_audio_processor_status(httpd_req_t *req)
{
    return send_audio_processor_status(req, ESP_OK);
}

static esp_err_t handle_api_audio_processor_probe(httpd_req_t *req)
{
    if (audio_service_is_busy()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"audio_busy\"}");
    }

    esp_err_t err = audio_processor_service_probe_once();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
    }
    return send_audio_processor_status(req, err);
}

static esp_err_t handle_api_audio_processor_aec_probe(httpd_req_t *req)
{
    if (audio_service_is_busy()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"audio_busy\"}");
    }

    esp_err_t err = audio_processor_service_aec_probe_once();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, err == ESP_ERR_NO_MEM
                                   ? "409 Conflict"
                                   : "500 Internal Server Error");
    }
    return send_audio_processor_status(req, err);
}

static esp_err_t handle_api_audio_processor_shadow_start(httpd_req_t *req)
{
    if (audio_service_is_busy()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"audio_busy\"}");
    }

    esp_err_t err = audio_processor_service_shadow_start();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, err == ESP_ERR_INVALID_STATE
                                   ? "409 Conflict"
                                   : "500 Internal Server Error");
    }
    return send_audio_processor_status(req, err);
}

static esp_err_t handle_api_audio_processor_shadow_stop(httpd_req_t *req)
{
    esp_err_t err = audio_processor_service_shadow_stop();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, err == ESP_ERR_INVALID_STATE
                                   ? "409 Conflict"
                                   : "500 Internal Server Error");
    }
    return send_audio_processor_status(req, err);
}

static esp_err_t handle_api_audio_processor_bridge_start(httpd_req_t *req)
{
    if (audio_service_is_busy()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"audio_busy\"}");
    }

    esp_err_t err = audio_processor_service_bridge_start();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, err == ESP_ERR_INVALID_STATE
                                   ? "409 Conflict"
                                   : "500 Internal Server Error");
    }
    return send_audio_processor_status(req, err);
}

static esp_err_t handle_api_audio_processor_bridge_stop(httpd_req_t *req)
{
    esp_err_t err = audio_processor_service_bridge_stop();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, err == ESP_ERR_INVALID_STATE
                                   ? "409 Conflict"
                                   : "500 Internal Server Error");
    }
    return send_audio_processor_status(req, err);
}

static esp_err_t send_audio_opus_worker_status(httpd_req_t *req, esp_err_t err)
{
    nb_opus_worker_status_t st;
    audio_processor_service_get_opus_worker_status(&st);

    char buf[1280];
    snprintf(buf, sizeof(buf),
             "{\"ok\":%s,\"ran\":%s,\"running\":%s,\"task_created\":%s,"
             "\"worker_ok\":%s,"
             "\"persistent\":%s,"
             "\"stop_requested\":%s,"
             "\"internal_before_kb\":%lu,"
             "\"internal_after_open_kb\":%lu,"
             "\"internal_after_close_kb\":%lu,"
             "\"dma_before_kb\":%lu,"
             "\"dma_after_open_kb\":%lu,"
             "\"dma_after_close_kb\":%lu,"
             "\"psram_before_kb\":%lu,"
             "\"psram_after_open_kb\":%lu,"
             "\"psram_after_close_kb\":%lu,"
             "\"frame_samples\":%d,"
             "\"bitrate\":%d,"
             "\"outbuf_bytes\":%d,"
             "\"encoded_bytes\":%d,"
             "\"encode_requests\":%lu,"
             "\"encode_packets\":%lu,"
             "\"encode_failures\":%lu,"
             "\"encoded_bytes_total\":%lu,"
             "\"pcm_feed_chunks\":%lu,"
             "\"pcm_feed_samples\":%lu,"
             "\"pcm_feed_frames\":%lu,"
             "\"pcm_feed_drops\":%lu,"
             "\"pcm_encode_packets\":%lu,"
             "\"pcm_encoded_bytes_total\":%lu,"
             "\"opus_packet_enqueued\":%lu,"
             "\"opus_packet_drops\":%lu,"
             "\"opus_packet_drained\":%lu,"
             "\"opus_packet_bytes_total\":%lu,"
             "\"opus_packet_queue_count\":%u,"
             "\"codec_error\":%d,"
             "\"last_error\":\"%s\","
             "\"probe_error\":\"%s\"}",
             (err == ESP_OK) ? "true" : "false",
             st.ran ? "true" : "false",
             st.running ? "true" : "false",
             st.task_created ? "true" : "false",
             st.ok ? "true" : "false",
             st.persistent ? "true" : "false",
             st.stop_requested ? "true" : "false",
             (unsigned long)st.internal_before_kb,
             (unsigned long)st.internal_after_open_kb,
             (unsigned long)st.internal_after_close_kb,
             (unsigned long)st.dma_before_kb,
             (unsigned long)st.dma_after_open_kb,
             (unsigned long)st.dma_after_close_kb,
             (unsigned long)st.psram_before_kb,
             (unsigned long)st.psram_after_open_kb,
             (unsigned long)st.psram_after_close_kb,
             st.frame_samples,
             st.bitrate,
             st.outbuf_bytes,
             st.encoded_bytes,
             (unsigned long)st.encode_requests,
             (unsigned long)st.encode_packets,
             (unsigned long)st.encode_failures,
             (unsigned long)st.encoded_bytes_total,
             (unsigned long)st.pcm_feed_chunks,
             (unsigned long)st.pcm_feed_samples,
             (unsigned long)st.pcm_feed_frames,
             (unsigned long)st.pcm_feed_drops,
             (unsigned long)st.pcm_encode_packets,
             (unsigned long)st.pcm_encoded_bytes_total,
             (unsigned long)st.opus_packet_enqueued,
             (unsigned long)st.opus_packet_drops,
             (unsigned long)st.opus_packet_drained,
             (unsigned long)st.opus_packet_bytes_total,
             (unsigned)st.opus_packet_queue_count,
             st.codec_error,
             esp_err_to_name(st.last_error),
             esp_err_to_name(err));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t send_audio_io_v2_status(httpd_req_t *req, esp_err_t err)
{
    nb_audio_io_v2_status_t st;
    audio_io_service_v2_get_status(&st);

    char buf[640];
    snprintf(buf, sizeof(buf),
             "{\"ok\":%s,\"initialized\":%s,\"probe_running\":%s,"
             "\"probe_duration_ms\":%lu,\"probe_elapsed_ms\":%lu,"
             "\"rx_frames\":%lu,\"tx_frames\":%lu,"
             "\"tx_silence_frames\":%lu,\"i2s_recoveries\":%lu,"
             "\"dropped_frames\":%lu,\"rms_last\":%lu,\"peak_last\":%lu,"
             "\"rms_max\":%lu,\"peak_max\":%lu,"
             "\"heap_internal_free_kb\":%lu,\"heap_dma_free_kb\":%lu,"
             "\"last_error\":\"%s\",\"error\":\"%s\"}",
             (err == ESP_OK) ? "true" : "false",
             st.initialized ? "true" : "false",
             st.probe_running ? "true" : "false",
             (unsigned long)st.probe_duration_ms,
             (unsigned long)st.probe_elapsed_ms,
             (unsigned long)st.rx_frames,
             (unsigned long)st.tx_frames,
             (unsigned long)st.tx_silence_frames,
             (unsigned long)st.i2s_recoveries,
             (unsigned long)st.dropped_frames,
             (unsigned long)st.rms_last,
             (unsigned long)st.peak_last,
             (unsigned long)st.rms_max,
             (unsigned long)st.peak_max,
             (unsigned long)st.heap_internal_free_kb,
             (unsigned long)st.heap_dma_free_kb,
             esp_err_to_name(st.last_error),
             esp_err_to_name(err));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_audio_io_v2_status(httpd_req_t *req)
{
    return send_audio_io_v2_status(req, ESP_OK);
}

static esp_err_t handle_api_audio_io_v2_probe(httpd_req_t *req)
{
    if (audio_service_is_busy()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"audio_busy\"}");
    }

    uint32_t duration_ms = 1000U;
    char body[MAX_BODY_LEN];
    int body_len = 0;
    if (recv_body(req, body, sizeof(body), &body_len) && body_len > 0U) {
        cJSON *root = cJSON_ParseWithLength(body, strlen(body));
        if (!root) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
            return ESP_OK;
        }
        uint32_t requested = get_json_u32(root, "duration_ms");
        if (requested > 0U) {
            duration_ms = requested;
        }
        cJSON_Delete(root);
    }

    esp_err_t err = audio_io_service_v2_probe_start(duration_ms);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, err == ESP_ERR_INVALID_ARG
                                   ? "400 Bad Request"
                                   : "409 Conflict");
    }
    return send_audio_io_v2_status(req, err);
}

static esp_err_t handle_api_audio_io_v2_probe_stop(httpd_req_t *req)
{
    esp_err_t err = audio_io_service_v2_probe_stop();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
    }
    return send_audio_io_v2_status(req, err);
}

static esp_err_t send_audio_playback_v2_status(httpd_req_t *req, esp_err_t err)
{
    nb_audio_playback_v2_status_t st;
    audio_playback_service_v2_get_status(&st);

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"ok\":%s,\"initialized\":%s,\"playing\":%s,"
             "\"stop_requested\":%s,\"probe_duration_ms\":%lu,"
             "\"probe_elapsed_ms\":%lu,\"queued_chunks\":%lu,"
             "\"played_chunks\":%lu,\"dropped_chunks\":%lu,"
             "\"cancel_count\":%lu,\"amplitude\":%lu,"
             "\"last_error\":\"%s\",\"error\":\"%s\"}",
             (err == ESP_OK) ? "true" : "false",
             st.initialized ? "true" : "false",
             st.playing ? "true" : "false",
             st.stop_requested ? "true" : "false",
             (unsigned long)st.probe_duration_ms,
             (unsigned long)st.probe_elapsed_ms,
             (unsigned long)st.queued_chunks,
             (unsigned long)st.played_chunks,
             (unsigned long)st.dropped_chunks,
             (unsigned long)st.cancel_count,
             (unsigned long)st.amplitude,
             esp_err_to_name(st.last_error),
             esp_err_to_name(err));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_audio_playback_v2_status(httpd_req_t *req)
{
    return send_audio_playback_v2_status(req, ESP_OK);
}

static esp_err_t handle_api_audio_playback_v2_probe(httpd_req_t *req)
{
    if (audio_service_is_busy()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"audio_busy\"}");
    }

    uint32_t duration_ms = 320U;
    uint16_t amplitude = 1200U;
    char body[MAX_BODY_LEN];
    int body_len = 0;
    if (recv_body(req, body, sizeof(body), &body_len) && body_len > 0) {
        cJSON *root = cJSON_ParseWithLength(body, strlen(body));
        if (!root) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
            return ESP_OK;
        }
        uint32_t requested_duration = get_json_u32(root, "duration_ms");
        uint32_t requested_amplitude = get_json_u32(root, "amplitude");
        if (requested_duration > 0U) {
            duration_ms = requested_duration;
        }
        if (requested_amplitude > 0U && requested_amplitude <= UINT16_MAX) {
            amplitude = (uint16_t)requested_amplitude;
        }
        cJSON_Delete(root);
    }

    esp_err_t err = audio_playback_service_v2_probe_start(duration_ms, amplitude);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, err == ESP_ERR_INVALID_ARG
                                   ? "400 Bad Request"
                                   : "409 Conflict");
    }
    return send_audio_playback_v2_status(req, err);
}

static esp_err_t handle_api_audio_playback_v2_stop(httpd_req_t *req)
{
    esp_err_t err = audio_playback_service_v2_probe_stop();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
    }
    return send_audio_playback_v2_status(req, err);
}

static esp_err_t send_voice_capture_v2_status(httpd_req_t *req, esp_err_t err)
{
    nb_voice_capture_v2_status_t st;
    voice_capture_session_v2_get_status(&st);

    char buf[768];
    snprintf(buf, sizeof(buf),
             "{\"ok\":%s,\"initialized\":%s,\"session_active\":%s,"
             "\"state\":\"%s\",\"source\":\"%s\",\"session_id\":%lu,"
             "\"voice_start_sent\":%s,\"voice_audio_sent\":%s,"
             "\"voice_end_sent\":%s,\"replay_duration_ms\":%lu,"
             "\"replay_elapsed_ms\":%lu,\"speech_elapsed_ms\":%lu,"
             "\"silence_elapsed_ms\":%lu,\"speech_frames\":%lu,"
             "\"silence_frames\":%lu,\"captured_samples\":%lu,"
             "\"dropped_frames\":%lu,\"last_error\":\"%s\","
             "\"error\":\"%s\"}",
             (err == ESP_OK) ? "true" : "false",
             st.initialized ? "true" : "false",
             st.session_active ? "true" : "false",
             capture_v2_state_name(st.state),
             capture_v2_source_name(st.source),
             (unsigned long)st.session_id,
             st.voice_start_sent ? "true" : "false",
             st.voice_audio_sent ? "true" : "false",
             st.voice_end_sent ? "true" : "false",
             (unsigned long)st.replay_duration_ms,
             (unsigned long)st.replay_elapsed_ms,
             (unsigned long)st.speech_elapsed_ms,
             (unsigned long)st.silence_elapsed_ms,
             (unsigned long)st.speech_frames,
             (unsigned long)st.silence_frames,
             (unsigned long)st.captured_samples,
             (unsigned long)st.dropped_frames,
             esp_err_to_name(st.last_error),
             esp_err_to_name(err));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_voice_capture_v2_status(httpd_req_t *req)
{
    return send_voice_capture_v2_status(req, ESP_OK);
}

static esp_err_t handle_api_voice_capture_v2_replay(httpd_req_t *req)
{
    if (audio_service_is_busy()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"audio_busy\"}");
    }

    uint32_t speech_ms = 640U;
    uint32_t silence_ms = NB_VOICE_CAPTURE_V2_END_SILENCE_MS;
    nb_voice_capture_v2_source_t source = NB_VOICE_CAPTURE_V2_SOURCE_DEBUG;
    char body[MAX_BODY_LEN];
    int body_len = 0;
    if (recv_body(req, body, sizeof(body), &body_len) && body_len > 0) {
        cJSON *root = cJSON_ParseWithLength(body, strlen(body));
        if (!root) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
            return ESP_OK;
        }
        uint32_t requested_speech = get_json_u32(root, "speech_ms");
        uint32_t requested_silence = get_json_u32(root, "silence_ms");
        const cJSON *speech_j = cJSON_GetObjectItemCaseSensitive(root, "speech_ms");
        const cJSON *silence_j = cJSON_GetObjectItemCaseSensitive(root, "silence_ms");
        const cJSON *source_j = cJSON_GetObjectItemCaseSensitive(root, "source");
        if (speech_j != NULL) {
            speech_ms = requested_speech;
        }
        if (silence_j != NULL) {
            silence_ms = requested_silence;
        }
        if (cJSON_IsString(source_j)) {
            source = capture_v2_source_from_str(source_j->valuestring);
        }
        cJSON_Delete(root);
    }

    esp_err_t err = voice_capture_session_v2_replay_start(source, speech_ms, silence_ms);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, err == ESP_ERR_INVALID_ARG
                                   ? "400 Bad Request"
                                   : "409 Conflict");
    }
    return send_voice_capture_v2_status(req, err);
}

static esp_err_t handle_api_voice_capture_v2_cancel(httpd_req_t *req)
{
    esp_err_t err = voice_capture_session_v2_cancel();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
    }
    return send_voice_capture_v2_status(req, err);
}

static esp_err_t handle_api_audio_opus_worker_status(httpd_req_t *req)
{
    return send_audio_opus_worker_status(req, ESP_OK);
}

static esp_err_t handle_api_audio_opus_worker_probe(httpd_req_t *req)
{
    if (audio_service_is_busy()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"audio_busy\"}");
    }

    esp_err_t err = audio_processor_service_opus_worker_probe_once();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, err == ESP_ERR_NO_MEM
                                   ? "409 Conflict"
                                   : "500 Internal Server Error");
    }
    return send_audio_opus_worker_status(req, err);
}

static esp_err_t handle_api_audio_opus_worker_start(httpd_req_t *req)
{
    if (audio_service_is_busy()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"audio_busy\"}");
    }

    esp_err_t err = audio_processor_service_opus_worker_start();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, err == ESP_ERR_INVALID_STATE
                                   ? "409 Conflict"
                                   : "500 Internal Server Error");
    }
    return send_audio_opus_worker_status(req, err);
}

static esp_err_t handle_api_audio_opus_worker_stop(httpd_req_t *req)
{
    esp_err_t err = audio_processor_service_opus_worker_stop();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, err == ESP_ERR_INVALID_STATE
                                   ? "409 Conflict"
                                   : "500 Internal Server Error");
    }
    return send_audio_opus_worker_status(req, err);
}

static esp_err_t handle_api_audio_opus_worker_encode_test(httpd_req_t *req)
{
    esp_err_t err = audio_processor_service_opus_worker_encode_test_once();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, err == ESP_ERR_INVALID_STATE
                                   ? "409 Conflict"
                                   : "500 Internal Server Error");
    }
    return send_audio_opus_worker_status(req, err);
}

static esp_err_t handle_api_audio_opus_worker_drain_packets(httpd_req_t *req)
{
    uint32_t packets = 0;
    uint32_t bytes = 0;
    esp_err_t err = audio_processor_service_opus_worker_drain_packets(&packets, &bytes);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
    }
    httpd_resp_set_type(req, "application/json");

    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"ok\":%s,\"drained_packets\":%lu,\"drained_bytes\":%lu,\"error\":\"%s\"}",
             (err == ESP_OK) ? "true" : "false",
             (unsigned long)packets,
             (unsigned long)bytes,
             esp_err_to_name(err));
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_audio_opus_transport_enable(httpd_req_t *req)
{
    esp_err_t err = audio_processor_service_opus_worker_start();
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        bridge_service_set_opus_enabled(true);
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
    }

    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"ok\":%s,\"opus_enabled\":%s,\"error\":\"%s\"}",
             (err == ESP_OK) ? "true" : "false",
             bridge_service_opus_is_enabled() ? "true" : "false",
             esp_err_to_name(err));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_audio_opus_transport_disable(httpd_req_t *req)
{
    bridge_service_set_opus_enabled(false);
    esp_err_t err = audio_processor_service_opus_worker_stop();
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;
    }

    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"ok\":%s,\"opus_enabled\":%s,\"error\":\"%s\"}",
             (err == ESP_OK) ? "true" : "false",
             bridge_service_opus_is_enabled() ? "true" : "false",
             esp_err_to_name(err));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static void sanitize_audio_scenario(const char *src, char *dst, size_t dst_len)
{
    size_t j = 0U;
    if (dst_len == 0U) return;

    if (!src || src[0] == '\0') {
        snprintf(dst, dst_len, "sample");
        return;
    }

    for (size_t i = 0U; src[i] != '\0' && j + 1U < dst_len; i++) {
        char c = src[i];
        bool ok = (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9');
        if (ok) {
            dst[j++] = c;
        } else if (c == '_' || c == '-' || c == ' ') {
            dst[j++] = '_';
        }
    }

    if (j == 0U) {
        snprintf(dst, dst_len, "sample");
    } else {
        dst[j] = '\0';
    }
}

static esp_err_t handle_api_audio_record(httpd_req_t *req)
{
    char body[MAX_BODY_LEN];
    if (!recv_body(req, body, sizeof(body), NULL)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength(body, strlen(body));
    if (!root) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_json\"}");
    }

    const cJSON *source_j = cJSON_GetObjectItemCaseSensitive(root, "source");
    const cJSON *scenario_j = cJSON_GetObjectItemCaseSensitive(root, "scenario");
    const cJSON *duration_j = cJSON_GetObjectItemCaseSensitive(root, "duration_s");

    const char *source_req = cJSON_IsString(source_j) ? source_j->valuestring : "raw";
    const char *scenario = cJSON_IsString(scenario_j) ? scenario_j->valuestring : "sample";
    uint32_t duration_s = 5U;
    if (cJSON_IsNumber(duration_j)) {
        int requested = duration_j->valueint;
        if (requested > 0 && requested <= 10) {
            duration_s = (uint32_t)requested;
        }
    }

    bool use_bridge_tx = strcmp(source_req, "bridge_tx") == 0;
    bool use_raw = strcmp(source_req, "raw") == 0;
    if (!use_raw && !use_bridge_tx) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_source\"}");
    }
    const char *source = use_bridge_tx ? "bridge_tx" : "raw";

    if (!sd_hal_is_mounted() && sd_hal_try_remount() != ESP_OK) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"sd_unmounted\"}");
    }

    if (audio_service_is_busy()) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"audio_busy\"}");
    }

    char scenario_clean[32];
    sanitize_audio_scenario(scenario, scenario_clean, sizeof(scenario_clean));
    (void)mkdir(NB_SD_MOUNT_POINT "/logs/audio", 0775);

    char path[96];
    uint32_t uptime_s = diagnostics_get_uptime_s();
    snprintf(path, sizeof(path), NB_SD_MOUNT_POINT "/logs/audio/%s_%s_%lus.wav",
             source, scenario_clean, (unsigned long)uptime_s);

    esp_err_t err = use_bridge_tx
                  ? audio_record_bridge_tx_diagnostic(path, duration_s)
                  : audio_record_diagnostic(path, duration_s);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        char resp[96];
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, err == ESP_ERR_INVALID_STATE ? "409 Conflict" : "400 Bad Request");
        return httpd_resp_sendstr(req, resp);
    }

    char resp[192];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"source\":\"%s\",\"duration_s\":%lu,\"path\":\"%s\"}",
             source, (unsigned long)duration_s, path);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static bool audio_sample_filename_valid(const char *name)
{
    size_t len = name ? strlen(name) : 0U;
    if (len < 5U || len > 80U) return false;
    if (strcmp(name + len - 4U, ".wav") != 0) return false;

    for (size_t i = 0U; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') ||
                  c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

static esp_err_t ensure_audio_sample_dir(void)
{
    if (!sd_hal_is_mounted() && sd_hal_try_remount() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    (void)mkdir(NB_SD_MOUNT_POINT "/logs/audio", 0775);
    return ESP_OK;
}

static esp_err_t handle_api_audio_files(httpd_req_t *req)
{
    if (ensure_audio_sample_dir() != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"sd_unmounted\"}");
    }

    DIR *dir = opendir(NB_SD_MOUNT_POINT "/logs/audio");
    if (!dir) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"open_dir_failed\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"ok\":true,\"files\":[");

    bool first = true;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        char name[96];
        if (strlcpy(name, entry->d_name, sizeof(name)) >= sizeof(name)) continue;
        if (!audio_sample_filename_valid(name)) continue;

        char path[160];
        snprintf(path, sizeof(path), NB_SD_MOUNT_POINT "/logs/audio/%s", name);

        struct stat st;
        long size = 0L;
        if (stat(path, &st) == 0) {
            size = (long)st.st_size;
        }

        char item[320];
        snprintf(item, sizeof(item),
                 "%s{\"name\":\"%s\",\"size\":%ld,\"path\":\"%s\"}",
                 first ? "" : ",", name, size, path);
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }

    closedir(dir);
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t handle_api_audio_file(httpd_req_t *req)
{
    char query[128];
    char name[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
        !audio_sample_filename_valid(name)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_name\"}");
    }

    if (ensure_audio_sample_dir() != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"sd_unmounted\"}");
    }

    char path[160];
    snprintf(path, sizeof(path), NB_SD_MOUNT_POINT "/logs/audio/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"not_found\"}");
    }

    char disposition[128];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", name);
    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    char chunk[1024];
    size_t n = 0U;
    while ((n = fread(chunk, 1U, sizeof(chunk), f)) > 0U) {
        esp_err_t err = httpd_resp_send_chunk(req, chunk, n);
        if (err != ESP_OK) {
            fclose(f);
            return err;
        }
    }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0U);
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

static void send_wifi_object(httpd_req_t *req, bool wrap)
{
    bool connected  = wifi_service_is_connected();
    const char *ip  = wifi_service_get_ip();
    const char *sid = wifi_service_get_ssid();
    int8_t rssi     = wifi_service_get_rssi();
    uint16_t reason = wifi_service_get_last_disconnect_reason();
    uint32_t disc_count = wifi_service_get_disconnect_count();
    char buf[192];
    snprintf(buf, sizeof(buf),
        "%s\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
        "\"last_disconnect_reason\":%u,\"disconnect_count\":%lu%s",
        wrap ? "{" : "",
        connected ? "true" : "false", sid, ip, (int)rssi,
        (unsigned)reason,
        (unsigned long)disc_count,
        wrap ? "}" : "");
    httpd_resp_sendstr_chunk(req, buf);
}

static esp_err_t handle_api_wifi_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    send_wifi_object(req, true);
    return httpd_resp_sendstr_chunk(req, NULL);
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

static void send_touch_object(httpd_req_t *req, bool wrap)
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
        "%s\"raw\":%lu,\"filtered\":%lu,\"baseline\":%lu,"
        "\"threshold_on\":%lu,\"threshold_off\":%lu,"
        "\"state\":\"%s\",\"pressed\":%s,\"duration_ms\":%lu,"
        "\"last_event\":\"%s\",\"last_event_age_ms\":%lld%s",
        wrap ? "{" : "",
        (unsigned long)dbg.raw,
        (unsigned long)dbg.filtered_raw,
        (unsigned long)dbg.baseline,
        (unsigned long)dbg.threshold_on,
        (unsigned long)dbg.threshold_off,
        touch_state_name(dbg.state),
        pressed ? "true" : "false",
        (unsigned long)dbg.touch_duration_ms,
        touch_event_name(last_event),
        (long long)age_ms,
        wrap ? "}" : "");
    httpd_resp_sendstr_chunk(req, buf);
}

static esp_err_t handle_api_touch_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    send_touch_object(req, true);
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* ── Agenda local: hora, timers, lembretes e alarmes ────────────────────── */

static void format_local_time(char *dst, size_t dst_len)
{
    struct tm tm_now;
    if (time_service_get_local_time(&tm_now) != ESP_OK) {
        if (dst_len > 0u) dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_len, "%04d-%02d-%02d %02d:%02d:%02d",
             tm_now.tm_year + 1900,
             tm_now.tm_mon + 1,
             tm_now.tm_mday,
             tm_now.tm_hour,
             tm_now.tm_min,
             tm_now.tm_sec);
}

static esp_err_t send_time_object(httpd_req_t *req, bool wrap)
{
    char local_time[24];
    char tz_name[NB_TIME_TZ_NAME_MAX * 2];
    char tz_posix[NB_TIME_TZ_POSIX_MAX * 2];
    char location[NB_TIME_LOCATION_MAX * 2];
    format_local_time(local_time, sizeof(local_time));
    json_escape(time_service_get_tz_name(), tz_name, sizeof(tz_name));
    json_escape(time_service_get_tz_posix(), tz_posix, sizeof(tz_posix));
    json_escape(time_service_get_location(), location, sizeof(location));

    time_t now = 0;
    time(&now);

    char chunk[96];
    if (wrap) {
        httpd_resp_sendstr_chunk(req, "{");
    }
    snprintf(chunk, sizeof(chunk), "\"synced\":%s,\"epoch\":%lld,",
             time_service_is_synced() ? "true" : "false",
             (long long)now);
    httpd_resp_sendstr_chunk(req, chunk);

    httpd_resp_sendstr_chunk(req, "\"timezone\":\"");
    httpd_resp_sendstr_chunk(req, tz_name);
    httpd_resp_sendstr_chunk(req, "\",\"posix_tz\":\"");
    httpd_resp_sendstr_chunk(req, tz_posix);
    httpd_resp_sendstr_chunk(req, "\",\"location_label\":\"");
    httpd_resp_sendstr_chunk(req, location);
    httpd_resp_sendstr_chunk(req, "\",\"local_time\":\"");
    httpd_resp_sendstr_chunk(req, local_time);
    httpd_resp_sendstr_chunk(req, "\"");
    if (wrap) {
        httpd_resp_sendstr_chunk(req, "}");
    }
    return ESP_OK;
}

static esp_err_t handle_api_time_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    send_time_object(req, true);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t handle_api_time_config(httpd_req_t *req)
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

    const cJSON *posix_j = cJSON_GetObjectItemCaseSensitive(root, "posix_tz");
    const cJSON *tz_j    = cJSON_GetObjectItemCaseSensitive(root, "timezone");
    const cJSON *loc_j   = cJSON_GetObjectItemCaseSensitive(root, "location_label");
    esp_err_t err = ESP_OK;

    if (cJSON_IsString(posix_j) || cJSON_IsString(tz_j)) {
        const char *posix = cJSON_IsString(posix_j) ? posix_j->valuestring
                                                    : time_service_get_tz_posix();
        const char *tz = cJSON_IsString(tz_j) ? tz_j->valuestring
                                              : time_service_get_tz_name();
        err = time_service_set_timezone(posix, tz);
    }
    if (err == ESP_OK && cJSON_IsString(loc_j)) {
        err = time_service_set_location(loc_j->valuestring);
    }
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    httpd_resp_set_status(req, "400 Bad Request");
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handle_api_time_sync(httpd_req_t *req)
{
    esp_err_t err = time_service_sync_now();
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) return httpd_resp_sendstr(req, "{\"ok\":true}");
    httpd_resp_set_status(req, "400 Bad Request");
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
    return httpd_resp_sendstr(req, buf);
}

static void send_timer_json(httpd_req_t *req, const nb_timer_t *t, bool first)
{
    char label[NB_AGENDA_LABEL_MAX * 2];
    char chunk[224];
    json_escape(t->label, label, sizeof(label));
    snprintf(chunk, sizeof(chunk),
        "%s{\"id\":%u,\"label\":\"%s\",\"duration_ms\":%lu,\"remaining_ms\":%lu}",
        first ? "" : ",",
        (unsigned)t->id,
        label,
        (unsigned long)t->duration_ms,
        (unsigned long)t->remaining_ms);
    httpd_resp_sendstr_chunk(req, chunk);
}

static void send_reminder_json(httpd_req_t *req, const nb_reminder_t *r, bool first)
{
    char label[NB_AGENDA_LABEL_MAX * 2];
    char chunk[224];
    json_escape(r->label, label, sizeof(label));
    snprintf(chunk, sizeof(chunk),
        "%s{\"id\":%u,\"text\":\"%s\",\"delay_ms\":%lu,\"remaining_ms\":%lu}",
        first ? "" : ",",
        (unsigned)r->id,
        label,
        (unsigned long)r->delay_ms,
        (unsigned long)r->remaining_ms);
    httpd_resp_sendstr_chunk(req, chunk);
}

static void send_alarm_json(httpd_req_t *req, const nb_alarm_t *a, bool first)
{
    char label[NB_AGENDA_LABEL_MAX * 2];
    char chunk[256];
    json_escape(a->label, label, sizeof(label));
    snprintf(chunk, sizeof(chunk),
        "%s{\"id\":%u,\"label\":\"%s\",\"hour\":%u,\"minute\":%u,"
        "\"weekdays_mask\":%u,\"enabled\":%s}",
        first ? "" : ",",
        (unsigned)a->id,
        label,
        (unsigned)a->hour,
        (unsigned)a->minute,
        (unsigned)a->days,
        a->enabled ? "true" : "false");
    httpd_resp_sendstr_chunk(req, chunk);
}

static void send_agenda_object(httpd_req_t *req, bool wrap)
{
    if (wrap) {
        httpd_resp_sendstr_chunk(req, "{");
    }
    httpd_resp_sendstr_chunk(req, "\"time\":{");
    send_time_object(req, false);
    httpd_resp_sendstr_chunk(req, "},\"timers\":[");
    bool first = true;
    for (uint8_t i = 0; i < NB_AGENDA_MAX_TIMERS; i++) {
        nb_timer_t t;
        if (agenda_timer_get(i, &t) && t.active) {
            send_timer_json(req, &t, first);
            first = false;
        }
    }
    httpd_resp_sendstr_chunk(req, "],\"reminders\":[");
    first = true;
    for (uint8_t i = 0; i < NB_AGENDA_MAX_REMINDERS; i++) {
        nb_reminder_t r;
        if (agenda_reminder_get(i, &r) && r.active) {
            send_reminder_json(req, &r, first);
            first = false;
        }
    }
    httpd_resp_sendstr_chunk(req, "],\"alarms\":[");
    first = true;
    for (uint8_t i = 0; i < NB_AGENDA_MAX_ALARMS; i++) {
        nb_alarm_t a;
        if (agenda_alarm_get(i, &a) && a.active) {
            send_alarm_json(req, &a, first);
            first = false;
        }
    }
    httpd_resp_sendstr_chunk(req, "]");
    if (wrap) {
        httpd_resp_sendstr_chunk(req, "}");
    }
}

static esp_err_t handle_api_agenda_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    send_agenda_object(req, true);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t agenda_send_result(httpd_req_t *req, esp_err_t err, uint8_t id)
{
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        char buf[32];
        snprintf(buf, sizeof(buf), "{\"ok\":true,\"id\":%u}", (unsigned)id);
        return httpd_resp_sendstr(req, buf);
    }
    httpd_resp_set_status(req, "400 Bad Request");
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
    return httpd_resp_sendstr(req, buf);
}

static cJSON *parse_agenda_body(httpd_req_t *req, char *body, size_t body_len)
{
    if (!recv_body(req, body, body_len, NULL)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return NULL;
    }
    cJSON *root = cJSON_ParseWithLength(body, strlen(body));
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    }
    return root;
}

static esp_err_t handle_api_timer_create(httpd_req_t *req)
{
    char body[MAX_BODY_LEN];
    cJSON *root = parse_agenda_body(req, body, sizeof(body));
    if (!root) return ESP_OK;
    uint32_t duration_ms = get_json_u32(root, "duration_ms");
    const cJSON *label_j = cJSON_GetObjectItemCaseSensitive(root, "label");
    const char *label = cJSON_IsString(label_j) ? label_j->valuestring : "timer";
    uint8_t id = 0;
    esp_err_t err = agenda_timer_create(duration_ms, label, &id);
    cJSON_Delete(root);
    return agenda_send_result(req, err, id);
}

static esp_err_t handle_api_timer_update(httpd_req_t *req)
{
    char body[MAX_BODY_LEN];
    cJSON *root = parse_agenda_body(req, body, sizeof(body));
    if (!root) return ESP_OK;
    uint8_t id = (uint8_t)get_json_u32(root, "id");
    uint32_t duration_ms = get_json_u32(root, "duration_ms");
    const cJSON *label_j = cJSON_GetObjectItemCaseSensitive(root, "label");
    const char *label = cJSON_IsString(label_j) ? label_j->valuestring : NULL;
    esp_err_t err = agenda_timer_update(id, duration_ms, label);
    cJSON_Delete(root);
    return agenda_send_result(req, err, id);
}

static esp_err_t handle_api_timer_cancel(httpd_req_t *req)
{
    char body[MAX_BODY_LEN];
    cJSON *root = parse_agenda_body(req, body, sizeof(body));
    if (!root) return ESP_OK;
    uint8_t id = (uint8_t)get_json_u32(root, "id");
    esp_err_t err = agenda_timer_cancel(id);
    cJSON_Delete(root);
    return agenda_send_result(req, err, id);
}

static esp_err_t handle_api_reminder_create(httpd_req_t *req)
{
    char body[MAX_BODY_LEN];
    cJSON *root = parse_agenda_body(req, body, sizeof(body));
    if (!root) return ESP_OK;
    uint32_t delay_ms = get_json_u32(root, "delay_ms");
    const cJSON *text_j = cJSON_GetObjectItemCaseSensitive(root, "text");
    const char *text = cJSON_IsString(text_j) ? text_j->valuestring : "";
    uint8_t id = 0;
    esp_err_t err = agenda_reminder_create(delay_ms, text, &id);
    cJSON_Delete(root);
    return agenda_send_result(req, err, id);
}

static esp_err_t handle_api_reminder_update(httpd_req_t *req)
{
    char body[MAX_BODY_LEN];
    cJSON *root = parse_agenda_body(req, body, sizeof(body));
    if (!root) return ESP_OK;
    uint8_t id = (uint8_t)get_json_u32(root, "id");
    uint32_t delay_ms = get_json_u32(root, "delay_ms");
    const cJSON *text_j = cJSON_GetObjectItemCaseSensitive(root, "text");
    const char *text = cJSON_IsString(text_j) ? text_j->valuestring : NULL;
    esp_err_t err = agenda_reminder_update(id, delay_ms, text);
    cJSON_Delete(root);
    return agenda_send_result(req, err, id);
}

static esp_err_t handle_api_reminder_cancel(httpd_req_t *req)
{
    char body[MAX_BODY_LEN];
    cJSON *root = parse_agenda_body(req, body, sizeof(body));
    if (!root) return ESP_OK;
    uint8_t id = (uint8_t)get_json_u32(root, "id");
    esp_err_t err = agenda_reminder_cancel(id);
    cJSON_Delete(root);
    return agenda_send_result(req, err, id);
}

static esp_err_t handle_api_alarm_create(httpd_req_t *req)
{
    char body[MAX_BODY_LEN];
    cJSON *root = parse_agenda_body(req, body, sizeof(body));
    if (!root) return ESP_OK;
    uint8_t hour = (uint8_t)get_json_u32(root, "hour");
    uint8_t minute = (uint8_t)get_json_u32(root, "minute");
    uint8_t mask = (uint8_t)get_json_u32(root, "weekdays_mask");
    const cJSON *enabled_j = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    bool enabled = !cJSON_IsBool(enabled_j) || cJSON_IsTrue(enabled_j);
    const cJSON *label_j = cJSON_GetObjectItemCaseSensitive(root, "label");
    const char *label = cJSON_IsString(label_j) ? label_j->valuestring : "alarme";
    uint8_t id = 0;
    esp_err_t err = agenda_alarm_create(hour, minute, mask, label, enabled, &id);
    cJSON_Delete(root);
    return agenda_send_result(req, err, id);
}

static esp_err_t handle_api_alarm_update(httpd_req_t *req)
{
    char body[MAX_BODY_LEN];
    cJSON *root = parse_agenda_body(req, body, sizeof(body));
    if (!root) return ESP_OK;
    uint8_t id = (uint8_t)get_json_u32(root, "id");
    uint8_t hour = (uint8_t)get_json_u32(root, "hour");
    uint8_t minute = (uint8_t)get_json_u32(root, "minute");
    uint8_t mask = (uint8_t)get_json_u32(root, "weekdays_mask");
    const cJSON *enabled_j = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    const cJSON *label_j = cJSON_GetObjectItemCaseSensitive(root, "label");
    const char *label = cJSON_IsString(label_j) ? label_j->valuestring : "alarme";
    esp_err_t err = agenda_alarm_update(id, hour, minute, mask, label);
    if (err == ESP_OK && cJSON_IsBool(enabled_j)) {
        err = agenda_alarm_set_enabled(id, cJSON_IsTrue(enabled_j));
    }
    cJSON_Delete(root);
    return agenda_send_result(req, err, id);
}

static esp_err_t handle_api_alarm_cancel(httpd_req_t *req)
{
    char body[MAX_BODY_LEN];
    cJSON *root = parse_agenda_body(req, body, sizeof(body));
    if (!root) return ESP_OK;
    uint8_t id = (uint8_t)get_json_u32(root, "id");
    esp_err_t err = agenda_alarm_cancel(id);
    cJSON_Delete(root);
    return agenda_send_result(req, err, id);
}

static esp_err_t handle_api_alarm_enabled(httpd_req_t *req)
{
    char body[MAX_BODY_LEN];
    cJSON *root = parse_agenda_body(req, body, sizeof(body));
    if (!root) return ESP_OK;
    uint8_t id = (uint8_t)get_json_u32(root, "id");
    const cJSON *enabled_j = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    esp_err_t err = cJSON_IsBool(enabled_j)
        ? agenda_alarm_set_enabled(id, cJSON_IsTrue(enabled_j))
        : ESP_ERR_INVALID_ARG;
    cJSON_Delete(root);
    return agenda_send_result(req, err, id);
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

static void send_ai_object(httpd_req_t *req, bool wrap)
{
    nb_ai_status_state_t ai;
    taskENTER_CRITICAL(&s_mux);
    ai = s_ai_state;
    taskEXIT_CRITICAL(&s_mux);

    uint32_t age_ms = UINT32_MAX;
    if (ai.updated_us > 0) {
        age_ms = (uint32_t)((esp_timer_get_time() - ai.updated_us) / 1000);
    }

    nb_bridge_transport_t bt = bridge_service_get_transport();
    uint32_t rx_age          = bridge_service_get_last_rx_age_ms();
    char rx_buf[16];
    char ai_age_buf[16];
    if (rx_age == UINT32_MAX) {
        snprintf(rx_buf, sizeof(rx_buf), "null");
    } else {
        snprintf(rx_buf, sizeof(rx_buf), "%lu", (unsigned long)rx_age);
    }
    if (age_ms == UINT32_MAX) {
        snprintf(ai_age_buf, sizeof(ai_age_buf), "null");
    } else {
        snprintf(ai_age_buf, sizeof(ai_age_buf), "%lu", (unsigned long)age_ms);
    }

    if (wrap) {
        httpd_resp_sendstr_chunk(req, "{");
    }

    char chunk[192];
    snprintf(chunk, sizeof(chunk),
        "\"connected\":%s,\"transport\":\"%s\","
        "\"protocol_v\":%u,\"last_rx_ms\":%s,",
        bridge_service_is_connected() ? "true" : "false",
        transport_name(bt),
        (unsigned)bridge_service_get_protocol_version(),
        rx_buf);
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
        "\"provider\":\"%s\",\"model\":\"%s\","
        "\"mode\":\"legacy\",\"route\":\"%s\",",
        ai.provider, ai.model, ai.route);
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
        "\"outcome\":\"%s\",\"detail\":\"%s\","
        "\"session_id\":%lu,\"updated_age_ms\":%s,",
        ai.outcome, ai.detail, (unsigned long)ai.session_id, ai_age_buf);
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
        "\"latency\":{\"stt_ms\":%lu,\"llm_ms\":%lu,\"tts_ms\":%lu},",
        (unsigned long)ai.stt_ms,
        (unsigned long)ai.llm_ms,
        (unsigned long)ai.tts_ms);
    httpd_resp_sendstr_chunk(req, chunk);

    httpd_resp_sendstr_chunk(req,
        "\"usage\":{\"input_tokens\":null,\"output_tokens\":null,\"estimated_cost\":null},"
        "\"api_key_configured\":null,"
        "\"fallback\":\"local_intents\"");
    if (wrap) {
        httpd_resp_sendstr_chunk(req, "}");
    }
}

static esp_err_t handle_api_ai_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    send_ai_object(req, true);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static const httpd_uri_t k_uris[] = {
    { .uri = "/api/status",  .method = HTTP_GET,  .handler = handle_api_status },
    { .uri = "/api/persona", .method = HTTP_GET,  .handler = handle_api_persona },
    { .uri = "/api/camera/status", .method = HTTP_GET, .handler = handle_api_camera_status },
    { .uri = "/api/camera/mode", .method = HTTP_POST, .handler = handle_api_camera_mode },
    { .uri = "/api/camera/session/close", .method = HTTP_POST, .handler = handle_api_camera_session_close },
    { .uri = "/api/camera/snapshot", .method = HTTP_GET, .handler = handle_api_camera_snapshot },
    { .uri = "/api/vision/status", .method = HTTP_GET, .handler = handle_api_vision_status },
    { .uri = "/api/vision/observe", .method = HTTP_GET, .handler = handle_api_vision_observe },
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
    { .uri = "/api/audio/io-v2", .method = HTTP_GET, .handler = handle_api_audio_io_v2_status },
    { .uri = "/api/audio/io-v2/probe", .method = HTTP_POST, .handler = handle_api_audio_io_v2_probe },
    { .uri = "/api/audio/io-v2/probe/stop", .method = HTTP_POST, .handler = handle_api_audio_io_v2_probe_stop },
    { .uri = "/api/audio/playback-v2", .method = HTTP_GET, .handler = handle_api_audio_playback_v2_status },
    { .uri = "/api/audio/playback-v2/probe", .method = HTTP_POST, .handler = handle_api_audio_playback_v2_probe },
    { .uri = "/api/audio/playback-v2/stop", .method = HTTP_POST, .handler = handle_api_audio_playback_v2_stop },
    { .uri = "/api/audio/capture-v2", .method = HTTP_GET, .handler = handle_api_voice_capture_v2_status },
    { .uri = "/api/audio/capture-v2/replay", .method = HTTP_POST, .handler = handle_api_voice_capture_v2_replay },
    { .uri = "/api/audio/capture-v2/cancel", .method = HTTP_POST, .handler = handle_api_voice_capture_v2_cancel },
    { .uri = "/api/audio/processor", .method = HTTP_GET,   .handler = handle_api_audio_processor_status },
    { .uri = "/api/audio/processor/probe", .method = HTTP_POST, .handler = handle_api_audio_processor_probe },
    { .uri = "/api/audio/processor/aec/probe", .method = HTTP_POST, .handler = handle_api_audio_processor_aec_probe },
    { .uri = "/api/audio/processor/shadow/start", .method = HTTP_POST, .handler = handle_api_audio_processor_shadow_start },
    { .uri = "/api/audio/processor/shadow/stop", .method = HTTP_POST, .handler = handle_api_audio_processor_shadow_stop },
    { .uri = "/api/audio/processor/bridge/start", .method = HTTP_POST, .handler = handle_api_audio_processor_bridge_start },
    { .uri = "/api/audio/processor/bridge/stop", .method = HTTP_POST, .handler = handle_api_audio_processor_bridge_stop },
    { .uri = "/api/audio/opus/worker", .method = HTTP_GET, .handler = handle_api_audio_opus_worker_status },
    { .uri = "/api/audio/opus/worker/probe", .method = HTTP_POST, .handler = handle_api_audio_opus_worker_probe },
    { .uri = "/api/audio/opus/worker/start", .method = HTTP_POST, .handler = handle_api_audio_opus_worker_start },
    { .uri = "/api/audio/opus/worker/stop", .method = HTTP_POST, .handler = handle_api_audio_opus_worker_stop },
    { .uri = "/api/audio/opus/worker/encode-test", .method = HTTP_POST, .handler = handle_api_audio_opus_worker_encode_test },
    { .uri = "/api/audio/opus/worker/drain-packets", .method = HTTP_POST, .handler = handle_api_audio_opus_worker_drain_packets },
    { .uri = "/api/audio/opus/transport/enable", .method = HTTP_POST, .handler = handle_api_audio_opus_transport_enable },
    { .uri = "/api/audio/opus/transport/disable", .method = HTTP_POST, .handler = handle_api_audio_opus_transport_disable },
    { .uri = "/api/audio/record",   .method = HTTP_POST,   .handler = handle_api_audio_record },
    { .uri = "/api/audio/files",    .method = HTTP_GET,    .handler = handle_api_audio_files },
    { .uri = "/api/audio/file",     .method = HTTP_GET,    .handler = handle_api_audio_file },
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
    /* Agenda local */
    { .uri = "/api/time",            .method = HTTP_GET,  .handler = handle_api_time_get },
    { .uri = "/api/time/config",     .method = HTTP_POST, .handler = handle_api_time_config },
    { .uri = "/api/time/sync",       .method = HTTP_POST, .handler = handle_api_time_sync },
    { .uri = "/api/agenda",          .method = HTTP_GET,  .handler = handle_api_agenda_get },
    { .uri = "/api/timer/create",    .method = HTTP_POST, .handler = handle_api_timer_create },
    { .uri = "/api/timer/update",    .method = HTTP_POST, .handler = handle_api_timer_update },
    { .uri = "/api/timer/cancel",    .method = HTTP_POST, .handler = handle_api_timer_cancel },
    { .uri = "/api/reminder/create", .method = HTTP_POST, .handler = handle_api_reminder_create },
    { .uri = "/api/reminder/update", .method = HTTP_POST, .handler = handle_api_reminder_update },
    { .uri = "/api/reminder/cancel", .method = HTTP_POST, .handler = handle_api_reminder_cancel },
    { .uri = "/api/alarm/create",    .method = HTTP_POST, .handler = handle_api_alarm_create },
    { .uri = "/api/alarm/update",    .method = HTTP_POST, .handler = handle_api_alarm_update },
    { .uri = "/api/alarm/cancel",    .method = HTTP_POST, .handler = handle_api_alarm_cancel },
    { .uri = "/api/alarm/enabled",   .method = HTTP_POST, .handler = handle_api_alarm_enabled },
    /* 12.23 — Diagnóstico de produto */
    { .uri = "/api/diag",               .method = HTTP_GET,  .handler = handle_api_diag },
    { .uri = "/api/diag/snapshot",      .method = HTTP_POST, .handler = handle_api_diag_snapshot },
    { .uri = "/api/diag/test/wake",     .method = HTTP_GET,  .handler = handle_api_diag_wake },
    { .uri = "/api/diag/test/bridge",   .method = HTTP_GET,  .handler = handle_api_diag_bridge },
    { .uri = "/api/ai",                 .method = HTTP_GET,  .handler = handle_api_ai_get },
};

/* ── Início do servidor ──────────────────────────────────────────────────── */

static void web_service_start(void)
{
    if (s_server) return;

    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets  = 3;
    cfg.max_uri_handlers  = 64;   /* APIs registradas + margem */
    cfg.server_port       = 80;
    cfg.stack_size        = HTTPD_TASK_STACK_SIZE;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;
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

    NB_LOGI(TAG, "HTTP API iniciado na porta 80");
}

static void web_service_stop(const char *reason)
{
    httpd_handle_t server = s_server;
    if (!server) return;

    s_server = NULL;
    esp_err_t err = httpd_stop(server);
    if (err != ESP_OK) {
        NB_LOGW(TAG, "httpd_stop falhou: %s", esp_err_to_name(err));
        return;
    }
    NB_LOGW(TAG, "HTTP server parado — %s", reason ? reason : "solicitado");
}

static void web_recover_task(void *arg)
{
    (void)arg;
    if (wifi_service_is_connected() && !s_server) {
        NB_LOGW(TAG, "watchdog HTTP: WiFi ativo sem servidor — reiniciando");
        web_service_start();
    }
    s_http_restart_pending = false;
    vTaskDelete(NULL);
}

static void web_health_cb(void *arg)
{
    (void)arg;
    if (wifi_service_is_connected() && !s_server) {
        if (!s_http_restart_pending) {
            s_http_restart_pending = true;
            BaseType_t ok;
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
            ok = xTaskCreateWithCaps(web_recover_task, "nb_web_recover",
                                     3072, NULL, 5, NULL,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
            ok = xTaskCreate(web_recover_task, "nb_web_recover",
                             3072, NULL, 5, NULL);
#endif
            if (ok != pdPASS) {
                s_http_restart_pending = false;
                NB_LOGW(TAG, "watchdog HTTP: falha ao criar task de recovery");
            }
        }
    }
}

/* ── Handlers de eventos ─────────────────────────────────────────────────── */

static void on_ip_acquired(const nb_event_t *ev, void *ctx)
{
    (void)ev; (void)ctx;
    web_service_start();
}

static void on_wifi_disconnected(const nb_event_t *ev, void *ctx)
{
    (void)ev; (void)ctx;
    web_service_stop("WiFi desconectado");
}

static void on_touch_debug_event(const nb_event_t *ev, void *ctx)
{
    (void)ctx;
    taskENTER_CRITICAL(&s_mux);
    s_last_touch_event = ev->type;
    s_last_touch_us    = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_mux);
}

static void on_bridge_session_event(const nb_event_t *ev, void *ctx)
{
    (void)ctx;
    const char *payload = (const char *)ev->data.ptr;
    if (!payload || !strstr(payload, "\"event\":\"AI_STATUS\"")) return;

    cJSON *root = cJSON_Parse(payload);
    if (!root) return;

    nb_ai_status_state_t next;
    taskENTER_CRITICAL(&s_mux);
    next = s_ai_state;
    taskEXIT_CRITICAL(&s_mux);

    copy_json_string(root, "provider", next.provider, sizeof(next.provider));
    copy_json_string(root, "model",    next.model,    sizeof(next.model));
    copy_json_string(root, "route",    next.route,    sizeof(next.route));
    copy_json_string(root, "outcome",  next.outcome,  sizeof(next.outcome));
    copy_json_string(root, "detail",   next.detail,   sizeof(next.detail));
    next.session_id = get_json_u32(root, "session_id");
    next.stt_ms     = get_json_u32(root, "stt_ms");
    next.llm_ms     = get_json_u32(root, "llm_ms");
    next.tts_ms     = get_json_u32(root, "tts_ms");
    next.updated_us = esp_timer_get_time();

    cJSON_Delete(root);

    taskENTER_CRITICAL(&s_mux);
    s_ai_state = next;
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

    if (!s_http_health_tmr) {
        const esp_timer_create_args_t th = {
            .callback = web_health_cb,
            .name     = "web_health",
        };
        esp_timer_create(&th, &s_http_health_tmr);
        esp_timer_start_periodic(s_http_health_tmr, 15000000LL);
    }

    /* Intercepta logs do ESP-IDF para ring buffer em RAM. */
    s_orig_vprintf = esp_log_set_vprintf(log_hook_vprintf);

    nb_event_subscribe(NB_EVT_WIFI_IP_ACQUIRED, on_ip_acquired,  NULL, NULL);
    nb_event_subscribe(NB_EVT_WIFI_DISCONNECTED, on_wifi_disconnected, NULL, NULL);
    nb_event_subscribe(NB_EVT_TOUCH_TAP,         on_touch_debug_event, NULL, NULL);
    nb_event_subscribe(NB_EVT_TOUCH_LONG_PRESS,  on_touch_debug_event, NULL, NULL);
    nb_event_subscribe(NB_EVT_TOUCH_SUSTAINED,   on_touch_debug_event, NULL, NULL);
    nb_event_subscribe(NB_EVT_TOUCH_WAKE,        on_touch_debug_event, NULL, NULL);
    nb_event_subscribe(NB_EVT_TOUCH_DOUBLE_TAP,  on_touch_debug_event, NULL, NULL);
    nb_event_subscribe(NB_EVT_TOUCH_DEEP,        on_touch_debug_event, NULL, NULL);
    nb_event_subscribe(NB_EVT_TOUCH_CARESS,      on_touch_debug_event, NULL, NULL);
    nb_event_subscribe(NB_EVT_TOUCH_WARM_PULSE,  on_touch_debug_event, NULL, NULL);
    nb_event_subscribe(NB_EVT_BRIDGE_SESSION,    on_bridge_session_event, NULL, NULL);
    if (wifi_service_is_connected()) {
        web_service_start();
    }
    NB_LOGI(TAG, "web_service registrado — aguardando IP");
    return ESP_OK;
}
