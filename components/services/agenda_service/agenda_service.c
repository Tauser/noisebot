/*
 * agenda_service.c — Timers, lembretes e alarmes locais (Layer 5)
 *
 * Thread-safety: portMUX apenas para alocação/liberação de slot (flag active).
 * O countdown de remaining_ms é lido/escrito exclusivamente pelo behavior_task
 * via agenda_service_tick() — sem lock nas atualizações de contagem.
 *
 * Alarmes NVS: namespace nb_alm, chaves a{id}{campo} (ex: "a0h", "a3l").
 * Timers e lembretes NÃO são persistidos (base monotônica, perdidos no reboot).
 */

#include "agenda_service.h"
#include "time_service.h"
#include "event_bus.h"
#include "nb_events.h"
#include "logger.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "nvs.h"
#include "esp_log.h"
#include "cJSON.h"

#include <ctype.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#define TAG    "nb_agenda"
#define NVS_NS "nb_alm"

/* ── Slots internos ──────────────────────────────────────────────────────── */

typedef struct {
    bool     active;
    char     label[NB_AGENDA_LABEL_MAX];
    uint32_t duration_ms;
    uint32_t remaining_ms;
} nb_timer_slot_t;

typedef struct {
    bool     active;
    char     label[NB_AGENDA_LABEL_MAX];
    uint32_t delay_ms;
    uint32_t remaining_ms;
} nb_reminder_slot_t;

typedef struct {
    bool    active;
    bool    enabled;
    uint8_t hour;
    uint8_t minute;
    uint8_t days;
    char    label[NB_AGENDA_LABEL_MAX];
    uint8_t last_h;       /* hora do último disparo; 0xFF = nunca */
    uint8_t last_m;       /* minuto do último disparo */
    int8_t  last_wday;    /* dia da semana do último disparo; -1 = nunca */
} nb_alarm_slot_t;

static struct {
    bool                initialized;
    nb_timer_slot_t    timers[NB_AGENDA_MAX_TIMERS];
    nb_reminder_slot_t reminders[NB_AGENDA_MAX_REMINDERS];
    nb_alarm_slot_t    alarms[NB_AGENDA_MAX_ALARMS];
} s;

static portMUX_TYPE  s_mux               = portMUX_INITIALIZER_UNLOCKED;
static uint16_t      s_last_alarm_minute  = UINT16_MAX; /* UINT16_MAX = nunca verificado */

/* ── NVS helpers para alarmes ────────────────────────────────────────────── */

static void alarm_nvs_save(uint8_t id)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    char key[6]; /* "a7l\0" = 4 chars máx */
    const nb_alarm_slot_t *a = &s.alarms[id];

    snprintf(key, sizeof(key), "a%dh", id); nvs_set_u8(h, key, a->hour);
    snprintf(key, sizeof(key), "a%dm", id); nvs_set_u8(h, key, a->minute);
    snprintf(key, sizeof(key), "a%dd", id); nvs_set_u8(h, key, a->days);
    snprintf(key, sizeof(key), "a%de", id); nvs_set_u8(h, key, a->enabled  ? 1U : 0U);
    snprintf(key, sizeof(key), "a%da", id); nvs_set_u8(h, key, a->active   ? 1U : 0U);
    snprintf(key, sizeof(key), "a%dl", id); nvs_set_str(h, key, a->label);

    nvs_commit(h);
    nvs_close(h);
}

static void alarm_nvs_load_all(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    for (uint8_t i = 0; i < NB_AGENDA_MAX_ALARMS; i++) {
        char    key[6];
        uint8_t val;

        snprintf(key, sizeof(key), "a%da", i);
        if (nvs_get_u8(h, key, &val) != ESP_OK) continue; /* slot nunca salvo */

        nb_alarm_slot_t *a = &s.alarms[i];
        a->active    = (val != 0U);
        a->last_h    = 0xFFU;
        a->last_m    = 0xFFU;
        a->last_wday = -1;

        snprintf(key, sizeof(key), "a%dh", i); nvs_get_u8(h, key, &a->hour);
        snprintf(key, sizeof(key), "a%dm", i); nvs_get_u8(h, key, &a->minute);
        snprintf(key, sizeof(key), "a%dd", i); nvs_get_u8(h, key, &a->days);

        snprintf(key, sizeof(key), "a%de", i);
        a->enabled = (nvs_get_u8(h, key, &val) == ESP_OK) ? (val != 0U) : true;

        snprintf(key, sizeof(key), "a%dl", i);
        size_t sz = NB_AGENDA_LABEL_MAX;
        nvs_get_str(h, key, a->label, &sz);

        if (a->active) {
            NB_LOGI(TAG, "alarme %u restaurado: '%s' %02d:%02d days=0x%02x",
                    (unsigned)i, a->label, a->hour, a->minute, a->days);
        }
    }

    nvs_close(h);
}

/* ── Tick de timers ──────────────────────────────────────────────────────── */

static void tick_timers(uint32_t dt_ms)
{
    for (uint8_t i = 0; i < NB_AGENDA_MAX_TIMERS; i++) {
        if (!s.timers[i].active) continue;

        nb_timer_slot_t *t = &s.timers[i];
        if (t->remaining_ms <= dt_ms) {
            t->remaining_ms = 0U;
            portENTER_CRITICAL(&s_mux);
            t->active = false;
            portEXIT_CRITICAL(&s_mux);

            NB_LOGI(TAG, "timer %u venceu: '%s'", (unsigned)i, t->label);
            nb_event_t evt = { .type = NB_EVT_TIMER_DONE, .data.u32 = (uint32_t)i };
            nb_event_publish_async(&evt);
        } else {
            t->remaining_ms -= dt_ms;
        }
    }
}

/* ── Tick de lembretes ───────────────────────────────────────────────────── */

static void tick_reminders(uint32_t dt_ms)
{
    for (uint8_t i = 0; i < NB_AGENDA_MAX_REMINDERS; i++) {
        if (!s.reminders[i].active) continue;

        nb_reminder_slot_t *r = &s.reminders[i];
        if (r->remaining_ms <= dt_ms) {
            r->remaining_ms = 0U;
            portENTER_CRITICAL(&s_mux);
            r->active = false;
            portEXIT_CRITICAL(&s_mux);

            NB_LOGI(TAG, "lembrete %u: '%s'", (unsigned)i, r->label);
            nb_event_t evt = { .type = NB_EVT_REMINDER_DUE, .data.u32 = (uint32_t)i };
            nb_event_publish_async(&evt);
        } else {
            r->remaining_ms -= dt_ms;
        }
    }
}

/* ── Verificação de alarmes (idempotente por minuto) ─────────────────────── */

static void check_alarms(void)
{
    if (!time_service_is_synced()) return;

    struct tm now;
    if (time_service_get_local_time(&now) != ESP_OK) return;

    uint16_t cur_min = (uint16_t)((uint16_t)now.tm_hour * 60U + (uint16_t)now.tm_min);
    if (cur_min == s_last_alarm_minute) return;
    s_last_alarm_minute = cur_min;

    for (uint8_t i = 0; i < NB_AGENDA_MAX_ALARMS; i++) {
        nb_alarm_slot_t *a = &s.alarms[i];
        if (!a->active || !a->enabled) continue;
        if (a->hour != (uint8_t)now.tm_hour) continue;
        if (a->minute != (uint8_t)now.tm_min) continue;

        if (a->days != 0U) {
            uint8_t day_bit = (uint8_t)(1U << now.tm_wday);
            if (!(a->days & day_bit)) continue;
        }

        /* Evita re-disparo se hora/minuto/dia já foram registrados */
        if (a->last_h    == (uint8_t)now.tm_hour &&
            a->last_m    == (uint8_t)now.tm_min  &&
            a->last_wday == (int8_t) now.tm_wday) continue;

        a->last_h    = (uint8_t)now.tm_hour;
        a->last_m    = (uint8_t)now.tm_min;
        a->last_wday = (int8_t) now.tm_wday;

        NB_LOGI(TAG, "alarme %u: '%s' %02d:%02d", (unsigned)i, a->label,
                a->hour, a->minute);

        nb_event_t evt = { .type = NB_EVT_ALARM_DUE, .data.u32 = (uint32_t)i };
        nb_event_publish_async(&evt);

        /* Disparo único: desativar e persistir */
        if (a->days == 0U) {
            portENTER_CRITICAL(&s_mux);
            a->active = false;
            portEXIT_CRITICAL(&s_mux);
            alarm_nvs_save(i);
        }
    }
}

/* ── Comandos vindos do bridge (MSG_SESSION) ─────────────────────────────── */

static uint32_t json_u32_or(cJSON *root, const char *key, uint32_t fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0.0) return fallback;
    return (uint32_t)item->valuedouble;
}

static const char *json_str_or(cJSON *root, const char *key, const char *fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || !item->valuestring) return fallback;
    return item->valuestring;
}

static bool label_equals_ci(const char *left, const char *right)
{
    if (!left || !right || left[0] == '\0' || right[0] == '\0') return false;
    while (*left != '\0' && *right != '\0') {
        unsigned char a = (unsigned char)*left++;
        unsigned char b = (unsigned char)*right++;
        if (tolower(a) != tolower(b)) return false;
    }
    return *left == '\0' && *right == '\0';
}

static bool find_timer_by_label(const char *label, uint8_t *out_id)
{
    if (!out_id) return false;
    for (uint8_t i = 0; i < NB_AGENDA_MAX_TIMERS; i++) {
        if (s.timers[i].active && label_equals_ci(s.timers[i].label, label)) {
            *out_id = i;
            return true;
        }
    }
    return false;
}

static bool find_reminder_by_label(const char *label, uint8_t *out_id)
{
    if (!out_id) return false;
    for (uint8_t i = 0; i < NB_AGENDA_MAX_REMINDERS; i++) {
        if (s.reminders[i].active && label_equals_ci(s.reminders[i].label, label)) {
            *out_id = i;
            return true;
        }
    }
    return false;
}

static bool find_alarm_by_label(const char *label, uint8_t *out_id)
{
    if (!out_id) return false;
    for (uint8_t i = 0; i < NB_AGENDA_MAX_ALARMS; i++) {
        if (s.alarms[i].active && label_equals_ci(s.alarms[i].label, label)) {
            *out_id = i;
            return true;
        }
    }
    return false;
}

static bool first_active_timer(uint8_t *out_id)
{
    if (!out_id) return false;
    for (uint8_t i = 0; i < NB_AGENDA_MAX_TIMERS; i++) {
        if (s.timers[i].active) { *out_id = i; return true; }
    }
    return false;
}

static void on_bridge_session_event(const nb_event_t *ev, void *ctx)
{
    (void)ctx;
    if (!ev || ev->type != NB_EVT_BRIDGE_SESSION || !ev->data.ptr) return;

    cJSON *root = cJSON_Parse((const char *)ev->data.ptr);
    if (!root) return;

    const char *event = json_str_or(root, "event", "");
    if (strcmp(event, "AGENDA_COMMAND") != 0) {
        cJSON_Delete(root);
        return;
    }

    const char *action = json_str_or(root, "action", "");
    const char *label  = json_str_or(root, "label", "");
    uint8_t id = (uint8_t)json_u32_or(root, "id", UINT32_MAX);
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;

    if (strcmp(action, "timer_create") == 0) {
        uint32_t duration_ms = json_u32_or(root, "duration_ms", 0U);
        err = agenda_timer_create(duration_ms, label[0] ? label : "timer", &id);
    } else if (strcmp(action, "timer_cancel") == 0) {
        if (id >= NB_AGENDA_MAX_TIMERS && !find_timer_by_label(label, &id)) {
            (void)first_active_timer(&id);
        }
        err = agenda_timer_cancel(id);
    } else if (strcmp(action, "reminder_create") == 0) {
        uint32_t delay_ms = json_u32_or(root, "delay_ms", 0U);
        err = agenda_reminder_create(delay_ms, label[0] ? label : "lembrete", &id);
    } else if (strcmp(action, "reminder_cancel") == 0) {
        if (id >= NB_AGENDA_MAX_REMINDERS) {
            (void)find_reminder_by_label(label, &id);
        }
        err = agenda_reminder_cancel(id);
    } else if (strcmp(action, "alarm_create") == 0) {
        uint8_t hour = (uint8_t)json_u32_or(root, "hour", 255U);
        uint8_t minute = (uint8_t)json_u32_or(root, "minute", 255U);
        uint8_t days = (uint8_t)json_u32_or(root, "weekdays_mask", 0U);
        const cJSON *enabled_j = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        bool enabled = !cJSON_IsBool(enabled_j) || cJSON_IsTrue(enabled_j);
        err = agenda_alarm_create(hour, minute, days, label[0] ? label : "alarme", enabled, &id);
    } else if (strcmp(action, "alarm_set_enabled") == 0) {
        if (id >= NB_AGENDA_MAX_ALARMS) {
            (void)find_alarm_by_label(label, &id);
        }
        const cJSON *enabled_j = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        bool enabled = cJSON_IsBool(enabled_j) && cJSON_IsTrue(enabled_j);
        err = agenda_alarm_set_enabled(id, enabled);
    } else if (strcmp(action, "alarm_cancel") == 0) {
        if (id >= NB_AGENDA_MAX_ALARMS) {
            (void)find_alarm_by_label(label, &id);
        }
        err = agenda_alarm_cancel(id);
    }

    if (err != ESP_OK) {
        NB_LOGW(TAG, "comando agenda bridge '%s' falhou: %s", action, esp_err_to_name(err));
    } else {
        NB_LOGI(TAG, "comando agenda bridge '%s' aplicado id=%u", action, (unsigned)id);
    }

    cJSON_Delete(root);
}

/* ── API pública ─────────────────────────────────────────────────────────── */

esp_err_t agenda_service_init(void)
{
    if (s.initialized) return ESP_ERR_INVALID_STATE;

    memset(&s, 0, sizeof(s));
    for (uint8_t i = 0; i < NB_AGENDA_MAX_ALARMS; i++) {
        s.alarms[i].last_h    = 0xFFU;
        s.alarms[i].last_m    = 0xFFU;
        s.alarms[i].last_wday = -1;
    }

    alarm_nvs_load_all();
    s.initialized = true;
    nb_event_subscribe(NB_EVT_BRIDGE_SESSION, on_bridge_session_event, NULL, NULL);
    NB_LOGI(TAG, "agenda_service inicializado");
    return ESP_OK;
}

void agenda_service_tick(uint32_t dt_ms)
{
    if (!s.initialized || dt_ms == 0U) return;
    tick_timers(dt_ms);
    tick_reminders(dt_ms);
    check_alarms();
}

/* ── Timer API ───────────────────────────────────────────────────────────── */

esp_err_t agenda_timer_create(uint32_t duration_ms, const char *label,
                              uint8_t *out_id)
{
    if (!label || !out_id || duration_ms == 0U) return ESP_ERR_INVALID_ARG;
    if (!s.initialized) return ESP_ERR_INVALID_STATE;

    portENTER_CRITICAL(&s_mux);
    uint8_t id = NB_AGENDA_MAX_TIMERS;
    for (uint8_t i = 0; i < NB_AGENDA_MAX_TIMERS; i++) {
        if (!s.timers[i].active) { id = i; break; }
    }
    if (id >= NB_AGENDA_MAX_TIMERS) { portEXIT_CRITICAL(&s_mux); return ESP_ERR_NO_MEM; }

    s.timers[id].active       = true;
    s.timers[id].duration_ms  = duration_ms;
    s.timers[id].remaining_ms = duration_ms;
    strlcpy(s.timers[id].label, label, NB_AGENDA_LABEL_MAX);
    portEXIT_CRITICAL(&s_mux);

    *out_id = id;
    NB_LOGI(TAG, "timer %u: '%s' %lus", (unsigned)id, label,
            (unsigned long)(duration_ms / 1000UL));

    nb_event_t evt = { .type = NB_EVT_TIMER_STARTED, .data.u32 = (uint32_t)id };
    nb_event_publish_async(&evt);
    return ESP_OK;
}

esp_err_t agenda_timer_update(uint8_t id, uint32_t new_duration_ms,
                              const char *label)
{
    if (id >= NB_AGENDA_MAX_TIMERS || new_duration_ms == 0U) return ESP_ERR_INVALID_ARG;
    if (!s.timers[id].active) return ESP_ERR_NOT_FOUND;

    portENTER_CRITICAL(&s_mux);
    s.timers[id].duration_ms  = new_duration_ms;
    s.timers[id].remaining_ms = new_duration_ms;
    if (label != NULL) {
        strlcpy(s.timers[id].label, label, NB_AGENDA_LABEL_MAX);
    }
    portEXIT_CRITICAL(&s_mux);

    nb_event_t evt = { .type = NB_EVT_TIMER_UPDATED, .data.u32 = (uint32_t)id };
    nb_event_publish_async(&evt);
    return ESP_OK;
}

esp_err_t agenda_timer_cancel(uint8_t id)
{
    if (id >= NB_AGENDA_MAX_TIMERS) return ESP_ERR_INVALID_ARG;
    if (!s.timers[id].active) return ESP_ERR_NOT_FOUND;

    portENTER_CRITICAL(&s_mux);
    s.timers[id].active = false;
    portEXIT_CRITICAL(&s_mux);

    NB_LOGI(TAG, "timer %u cancelado", (unsigned)id);
    nb_event_t evt = { .type = NB_EVT_TIMER_CANCELLED, .data.u32 = (uint32_t)id };
    nb_event_publish_async(&evt);
    return ESP_OK;
}

bool agenda_timer_get(uint8_t id, nb_timer_t *out)
{
    if (id >= NB_AGENDA_MAX_TIMERS || !out) return false;
    const nb_timer_slot_t *t = &s.timers[id];
    out->id           = id;
    out->active       = t->active;
    out->duration_ms  = t->duration_ms;
    out->remaining_ms = t->remaining_ms;
    strlcpy(out->label, t->label, NB_AGENDA_LABEL_MAX);
    return true;
}

/* ── Reminder API ────────────────────────────────────────────────────────── */

esp_err_t agenda_reminder_create(uint32_t delay_ms, const char *label,
                                 uint8_t *out_id)
{
    if (!label || !out_id || delay_ms == 0U) return ESP_ERR_INVALID_ARG;
    if (!s.initialized) return ESP_ERR_INVALID_STATE;

    portENTER_CRITICAL(&s_mux);
    uint8_t id = NB_AGENDA_MAX_REMINDERS;
    for (uint8_t i = 0; i < NB_AGENDA_MAX_REMINDERS; i++) {
        if (!s.reminders[i].active) { id = i; break; }
    }
    if (id >= NB_AGENDA_MAX_REMINDERS) { portEXIT_CRITICAL(&s_mux); return ESP_ERR_NO_MEM; }

    s.reminders[id].active       = true;
    s.reminders[id].delay_ms     = delay_ms;
    s.reminders[id].remaining_ms = delay_ms;
    strlcpy(s.reminders[id].label, label, NB_AGENDA_LABEL_MAX);
    portEXIT_CRITICAL(&s_mux);

    *out_id = id;
    NB_LOGI(TAG, "lembrete %u: '%s' em %lus", (unsigned)id, label,
            (unsigned long)(delay_ms / 1000UL));

    nb_event_t evt = { .type = NB_EVT_REMINDER_SCHEDULED, .data.u32 = (uint32_t)id };
    nb_event_publish_async(&evt);
    return ESP_OK;
}

esp_err_t agenda_reminder_update(uint8_t id, uint32_t new_delay_ms,
                                 const char *label)
{
    if (id >= NB_AGENDA_MAX_REMINDERS || new_delay_ms == 0U) return ESP_ERR_INVALID_ARG;
    if (!s.reminders[id].active) return ESP_ERR_NOT_FOUND;

    portENTER_CRITICAL(&s_mux);
    s.reminders[id].delay_ms     = new_delay_ms;
    s.reminders[id].remaining_ms = new_delay_ms;
    if (label != NULL) {
        strlcpy(s.reminders[id].label, label, NB_AGENDA_LABEL_MAX);
    }
    portEXIT_CRITICAL(&s_mux);

    nb_event_t evt = { .type = NB_EVT_REMINDER_UPDATED, .data.u32 = (uint32_t)id };
    nb_event_publish_async(&evt);
    return ESP_OK;
}

esp_err_t agenda_reminder_cancel(uint8_t id)
{
    if (id >= NB_AGENDA_MAX_REMINDERS) return ESP_ERR_INVALID_ARG;
    if (!s.reminders[id].active) return ESP_ERR_NOT_FOUND;

    portENTER_CRITICAL(&s_mux);
    s.reminders[id].active = false;
    portEXIT_CRITICAL(&s_mux);

    NB_LOGI(TAG, "lembrete %u cancelado", (unsigned)id);
    nb_event_t evt = { .type = NB_EVT_REMINDER_CANCELLED, .data.u32 = (uint32_t)id };
    nb_event_publish_async(&evt);
    return ESP_OK;
}

bool agenda_reminder_get(uint8_t id, nb_reminder_t *out)
{
    if (id >= NB_AGENDA_MAX_REMINDERS || !out) return false;
    const nb_reminder_slot_t *r = &s.reminders[id];
    out->id           = id;
    out->active       = r->active;
    out->delay_ms     = r->delay_ms;
    out->remaining_ms = r->remaining_ms;
    strlcpy(out->label, r->label, NB_AGENDA_LABEL_MAX);
    return true;
}

/* ── Alarm API ───────────────────────────────────────────────────────────── */

esp_err_t agenda_alarm_create(uint8_t hour, uint8_t minute, uint8_t days,
                              const char *label, bool enabled,
                              uint8_t *out_id)
{
    if (!label || !out_id) return ESP_ERR_INVALID_ARG;
    if (hour > 23U || minute > 59U || days > 0x7FU) return ESP_ERR_INVALID_ARG;
    if (!s.initialized) return ESP_ERR_INVALID_STATE;

    portENTER_CRITICAL(&s_mux);
    uint8_t id = NB_AGENDA_MAX_ALARMS;
    for (uint8_t i = 0; i < NB_AGENDA_MAX_ALARMS; i++) {
        if (!s.alarms[i].active) { id = i; break; }
    }
    if (id >= NB_AGENDA_MAX_ALARMS) { portEXIT_CRITICAL(&s_mux); return ESP_ERR_NO_MEM; }

    nb_alarm_slot_t *a = &s.alarms[id];
    a->active    = true;
    a->enabled   = enabled;
    a->hour      = hour;
    a->minute    = minute;
    a->days      = days;
    a->last_h    = 0xFFU;
    a->last_m    = 0xFFU;
    a->last_wday = -1;
    strlcpy(a->label, label, NB_AGENDA_LABEL_MAX);
    portEXIT_CRITICAL(&s_mux);

    *out_id = id;
    NB_LOGI(TAG, "alarme %u: '%s' %02d:%02d days=0x%02x",
            (unsigned)id, label, hour, minute, days);
    alarm_nvs_save(id);

    nb_event_t evt = { .type = NB_EVT_ALARM_SCHEDULED, .data.u32 = (uint32_t)id };
    nb_event_publish_async(&evt);
    return ESP_OK;
}

esp_err_t agenda_alarm_update(uint8_t id, uint8_t hour, uint8_t minute,
                              uint8_t days, const char *label)
{
    if (id >= NB_AGENDA_MAX_ALARMS || !label) return ESP_ERR_INVALID_ARG;
    if (hour > 23U || minute > 59U || days > 0x7FU) return ESP_ERR_INVALID_ARG;
    if (!s.alarms[id].active) return ESP_ERR_NOT_FOUND;

    portENTER_CRITICAL(&s_mux);
    nb_alarm_slot_t *a = &s.alarms[id];
    a->hour      = hour;
    a->minute    = minute;
    a->days      = days;
    a->last_h    = 0xFFU; /* reset: não bloquear próximo disparo no novo horário */
    a->last_m    = 0xFFU;
    a->last_wday = -1;
    strlcpy(a->label, label, NB_AGENDA_LABEL_MAX);
    portEXIT_CRITICAL(&s_mux);

    alarm_nvs_save(id);
    nb_event_t evt = { .type = NB_EVT_ALARM_UPDATED, .data.u32 = (uint32_t)id };
    nb_event_publish_async(&evt);
    return ESP_OK;
}

esp_err_t agenda_alarm_cancel(uint8_t id)
{
    if (id >= NB_AGENDA_MAX_ALARMS) return ESP_ERR_INVALID_ARG;
    if (!s.alarms[id].active) return ESP_ERR_NOT_FOUND;

    portENTER_CRITICAL(&s_mux);
    s.alarms[id].active = false;
    portEXIT_CRITICAL(&s_mux);

    alarm_nvs_save(id);
    NB_LOGI(TAG, "alarme %u cancelado", (unsigned)id);
    nb_event_t evt = { .type = NB_EVT_ALARM_CANCELLED, .data.u32 = (uint32_t)id };
    nb_event_publish_async(&evt);
    return ESP_OK;
}

esp_err_t agenda_alarm_set_enabled(uint8_t id, bool enabled)
{
    if (id >= NB_AGENDA_MAX_ALARMS) return ESP_ERR_INVALID_ARG;
    if (!s.alarms[id].active) return ESP_ERR_NOT_FOUND;

    s.alarms[id].enabled = enabled;
    alarm_nvs_save(id);

    nb_event_t evt = { .type = NB_EVT_ALARM_UPDATED, .data.u32 = (uint32_t)id };
    nb_event_publish_async(&evt);
    return ESP_OK;
}

bool agenda_alarm_get(uint8_t id, nb_alarm_t *out)
{
    if (id >= NB_AGENDA_MAX_ALARMS || !out) return false;
    const nb_alarm_slot_t *a = &s.alarms[id];
    out->id      = id;
    out->active  = a->active;
    out->enabled = a->enabled;
    out->hour    = a->hour;
    out->minute  = a->minute;
    out->days    = a->days;
    strlcpy(out->label, a->label, NB_AGENDA_LABEL_MAX);
    return true;
}
