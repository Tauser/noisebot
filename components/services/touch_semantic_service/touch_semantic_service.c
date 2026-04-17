/*
 * touch_semantic_service.c — Touch semântico (Layer 5)
 *
 * Double-tap: primeiro TAP inicia janela de 400ms. Segundo TAP dentro da
 * janela → DOUBLE_TAP (TAP único suprimido). Sem segundo TAP → TAP publicado.
 *
 * Sustained progression (poll em tick via touch_service_get_duration_ms):
 *   3000–8000ms: NB_EVT_TOUCH_WARM_PULSE a cada 1s
 *   ≥ 8000ms:    NB_EVT_TOUCH_DEEP (uma vez)
 *   ≥ 15000ms:   NB_EVT_TOUCH_CARESS (uma vez)
 *
 * Thread safety: flags touch_task→tick protegidas por portMUX spinlock.
 */

#include "touch_semantic_service.h"

#include "event_bus.h"
#include "nb_events.h"
#include "touch_service.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_log.h"
#include "synth_service.h"

#define TAG "nb_touchsem"

#define DOUBLE_TAP_WINDOW_MS  400U
#define WARM_PULSE_PERIOD_MS  1000U
#define PULSE_START_MS        3000U
#define PULSE_END_MS          8000U
#define DEEP_MS               8000U
#define CARESS_MS             15000U

/* ── Flags dispatcher→tick ───────────────────────────────────────────────── */
static volatile bool s_tap_flag;
static volatile bool s_sustained_flag;
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;

/* ── Estado ──────────────────────────────────────────────────────────────── */
static bool     s_initialized;
static uint32_t s_uptime_ms;

/* Double-tap */
static bool     s_tap_pending;
static uint32_t s_tap_pending_ms;  /* countdown para single-tap publish */

/* Sustained progression */
static bool     s_sustained_active;
static bool     s_deep_fired;
static bool     s_caress_fired;
static uint32_t s_pulse_ms;        /* countdown para próximo WARM_PULSE */

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void publish(nb_event_type_t type)
{
    nb_event_t ev = { .type = type };
    nb_event_publish(&ev);
}

/* ── API chamada da touch_task ───────────────────────────────────────────── */

void touch_semantic_on_tap(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_tap_flag = true;
    taskEXIT_CRITICAL(&s_mux);
}

void touch_semantic_on_sustained(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_sustained_flag = true;
    taskEXIT_CRITICAL(&s_mux);
}

/* ── API ─────────────────────────────────────────────────────────────────── */

esp_err_t touch_semantic_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    s_initialized = true;
    ESP_LOGI(TAG, "touch_semantic_service inicializado");
    return ESP_OK;
}

void touch_semantic_tick(uint32_t dt_ms)
{
    if (!s_initialized) return;

    s_uptime_ms += dt_ms;

    /* Consumir flags */
    bool got_tap = false, got_sustained = false;
    taskENTER_CRITICAL(&s_mux);
    got_tap       = s_tap_flag;       s_tap_flag       = false;
    got_sustained = s_sustained_flag; s_sustained_flag = false;
    taskEXIT_CRITICAL(&s_mux);

    /* ── Double-tap ────────────────────────────────────────────────────────── */
    if (got_tap) {
        if (s_tap_pending) {
            /* Segundo tap dentro da janela → DOUBLE_TAP */
            s_tap_pending    = false;
            s_tap_pending_ms = 0;
            ESP_LOGD(TAG, "TOUCH_DOUBLE_TAP");
            publish(NB_EVT_TOUCH_DOUBLE_TAP);
        } else {
            /* Primeiro tap: aguarda possível segundo */
            s_tap_pending    = true;
            s_tap_pending_ms = DOUBLE_TAP_WINDOW_MS;
        }
    }

    /* Expiração da janela: publica single TAP */
    if (s_tap_pending) {
        if (s_tap_pending_ms <= dt_ms) {
            s_tap_pending    = false;
            s_tap_pending_ms = 0;
            ESP_LOGD(TAG, "TOUCH_TAP (single)");
            publish(NB_EVT_TOUCH_TAP);
        } else {
            s_tap_pending_ms -= dt_ms;
        }
    }

    /* ── Sustained: início ───────────────────────────────────────────────── */
    if (got_sustained) {
        s_sustained_active = true;
        s_deep_fired       = false;
        s_caress_fired     = false;
        s_pulse_ms         = WARM_PULSE_PERIOD_MS;
        publish(NB_EVT_TOUCH_SUSTAINED);
        ESP_LOGD(TAG, "TOUCH_SUSTAINED");
    }

    /* ── Sustained: progressão ───────────────────────────────────────────── */
    if (s_sustained_active) {
        if (!touch_service_is_pressed()) {
            s_sustained_active = false;
            s_deep_fired       = false;
            s_caress_fired     = false;
        } else {
            uint32_t dur = touch_service_get_duration_ms();

            /* WARM_PULSE a cada 1s durante 3–8s */
            if (dur >= PULSE_START_MS && dur < PULSE_END_MS) {
                if (s_pulse_ms <= dt_ms) {
                    s_pulse_ms = WARM_PULSE_PERIOD_MS;
                    publish(NB_EVT_TOUCH_WARM_PULSE);
                    ESP_LOGD(TAG, "TOUCH_WARM_PULSE (dur=%lums)", (unsigned long)dur);
                } else {
                    s_pulse_ms -= dt_ms;
                }
            }

            /* DEEP: uma vez a partir de 8s */
            if (!s_deep_fired && dur >= DEEP_MS) {
                s_deep_fired = true;
                publish(NB_EVT_TOUCH_DEEP);
                synth_purr(3000U, 0.60f);
                ESP_LOGI(TAG, "TOUCH_DEEP");
            }

            /* CARESS: uma vez a partir de 15s */
            if (!s_caress_fired && dur >= CARESS_MS) {
                s_caress_fired = true;
                publish(NB_EVT_TOUCH_CARESS);
                synth_purr(6000U, 0.80f);
                ESP_LOGI(TAG, "TOUCH_CARESS");
            }
        }
    }
}
