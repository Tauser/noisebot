/*
 * presence_semantic_service.c — Presença social anônima (Layer 5)
 *
 * Consome eventos brutos de visão (face box do servidor via bridge,
 * heurística local do vision_service) e publica eventos semânticos
 * NB_EVT_SOCIAL_PRESENCE_* no event bus para consumo por behavior_engine,
 * conductor e state_machine.
 *
 * Dois sensores fundidos (offline-first):
 *   face_box válido (w>0)  → presença CONFIRMADA (fonte mais forte)
 *   heurística local alta  → presença PROVÁVEL (sustenta estados, não dispara
 *                            eventos de retorno — evita ping falso)
 *
 * Bridge desconecta durante PRESENT/ENGAGED:
 *   face_box fica inválido; confiança rebaixa para "provável" sem
 *   evento de saída. Se heurística ainda alta: permanece em estado.
 *   Na reconexão, o primeiro face_box válido restaura confiança — não
 *   é ACQUIRED nem RETURNED (continuidade de presença).
 *
 * Histerese semântica garantida pelo serviço: consumidores podem confiar
 * nos eventos sem re-filtrar.
 *
 * Thread: callbacks de evento via event bus (qualquer task).
 *         Tick exclusivo do behavior_task (prio 5) a cada 100ms.
 *         Flags de sinal protegidas por spinlock; timers só no tick.
 */
#include "presence_semantic_service.h"

#include "event_bus.h"
#include "nb_events.h"
#include "bridge_service.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_log.h"
#include "esp_timer.h"

#define TAG "nb_presence"

/* Timers de transição (ms) — derivados da spec §2.3 */
#define HOLD_MS              5000U    /* hold antes de LEFT_RECENTLY              */
#define MAYBE_TIMEOUT_MS     5000U    /* MAYBE_SOMEONE sem confirmação → NO_ONE   */
#define HEURISTIC_CONFIRM_MS 10000U   /* heurística sustentada offline → PRESENT  */
#define LEFT_RECENTLY_MS     30000U   /* LEFT_RECENTLY → AWAY                     */
#define AWAY_MS             120000U   /* AWAY → ALONE_SETTLED (após LEFT_RECENTLY) */
#define ENGAGED_MS          180000U   /* PRESENT → ENGAGED (3 min)                */
#define RETURNED_MIN_MS      60000U   /* ausência mínima para evento RETURNED      */

static const char * const k_state_names[7] = {
    "NO_ONE", "MAYBE", "PRESENT", "ENGAGED",
    "LEFT_RECENTLY", "AWAY", "SETTLED"
};

/* ── Estado ──────────────────────────────────────────────────────────────── */

static presence_state_t s_state          = PRESENCE_NO_ONE;
static volatile bool    s_face_box_valid = false; /* face box w>0 do servidor  */
static volatile bool    s_heuristic_high = false; /* heurística local alta     */
static portMUX_TYPE     s_mux            = portMUX_INITIALIZER_UNLOCKED;

static bool s_confidence_confirmed = false; /* true=face box; false=heurística */

/* Soft timers (ms, só atualizados no tick) */
static uint32_t s_hold_ms          = 0U; /* hold antes de LEFT_RECENTLY          */
static uint32_t s_maybe_elapsed_ms = 0U; /* tempo em MAYBE_SOMEONE               */
static uint32_t s_heur_conf_ms     = 0U; /* heurística sustentada para confirmar  */
static uint32_t s_engaged_ms       = 0U; /* tempo em PRESENT acumulando p/ENGAGED */
static uint32_t s_since_left_ms    = 0U; /* tempo total desde entrada LEFT_RECENTLY */

/* Session / diagnóstico */
static uint32_t s_presence_session_ms = 0U;
static uint32_t s_absence_ms          = 0U;
static int64_t  s_last_seen_at_ms     = 0;
static uint32_t s_confirmed_count     = 0U;
static uint32_t s_returned_count      = 0U;
static uint32_t s_away_count          = 0U;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void publish_transition(nb_event_type_t type,
                                presence_state_t old_s,
                                presence_state_t new_s)
{
    nb_event_t ev = {
        .type = type,
        .data = { .u32 = ((uint32_t)old_s << 8U) | (uint32_t)new_s },
    };
    nb_event_publish(&ev);
    ESP_LOGI(TAG, "%s → %s", k_state_names[old_s], k_state_names[new_s]);
}

/* ── Event callbacks (chamados de qualquer task via event bus) ───────────── */

static void on_face_box(const nb_event_t *evt, void *ctx)
{
    (void)ctx;
    const nb_bridge_face_box_t *box = (const nb_bridge_face_box_t *)evt->data.ptr;
    bool valid = box && box->width > 0 && box->height > 0;
    taskENTER_CRITICAL(&s_mux);
    s_face_box_valid = valid;
    if (valid) {
        s_confidence_confirmed = true;
        s_last_seen_at_ms = (int64_t)(esp_timer_get_time() / 1000LL);
    }
    taskEXIT_CRITICAL(&s_mux);
}

static void on_presence_detected(const nb_event_t *evt, void *ctx)
{
    (void)ctx; (void)evt;
    taskENTER_CRITICAL(&s_mux);
    s_heuristic_high = true;
    if (!s_confidence_confirmed)
        s_last_seen_at_ms = (int64_t)(esp_timer_get_time() / 1000LL);
    taskEXIT_CRITICAL(&s_mux);
}

static void on_presence_lost(const nb_event_t *evt, void *ctx)
{
    (void)ctx; (void)evt;
    taskENTER_CRITICAL(&s_mux);
    s_heuristic_high = false;
    taskEXIT_CRITICAL(&s_mux);
}

static void on_bridge_disconnected(const nb_event_t *evt, void *ctx)
{
    (void)ctx; (void)evt;
    /* Rebaixa confiança para heurística sem transicionar estado.
     * Se heurística também estiver baixa, o hold timer tratará a saída. */
    taskENTER_CRITICAL(&s_mux);
    s_face_box_valid       = false;
    s_confidence_confirmed = false;
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGD(TAG, "bridge desconectado — face_box degradado para heurística");
}

/* ── Tick (behavior_task, 100ms) ─────────────────────────────────────────── */

void presence_semantic_tick(uint32_t dt_ms)
{
    taskENTER_CRITICAL(&s_mux);
    bool face_valid = s_face_box_valid;
    bool heur_high  = s_heuristic_high;
    taskEXIT_CRITICAL(&s_mux);

    bool signal = face_valid || heur_high;

    /* Acumula tempo de sessão e ausência para diagnóstico */
    if (s_state == PRESENCE_PRESENT || s_state == PRESENCE_ENGAGED) {
        s_presence_session_ms += dt_ms;
        s_absence_ms = 0U;
    } else if (!signal) {
        s_absence_ms += dt_ms;
    } else {
        s_absence_ms = 0U;
    }

    switch (s_state) {

    /* ── NO_ONE / ALONE_SETTLED ─────────────────────────────────────────── */
    case PRESENCE_NO_ONE:
    case PRESENCE_ALONE_SETTLED:
        if (signal) {
            presence_state_t old = s_state;
            s_state              = PRESENCE_MAYBE_SOMEONE;
            s_maybe_elapsed_ms   = 0U;
            s_heur_conf_ms       = 0U;
            publish_transition(NB_EVT_SOCIAL_PRESENCE_ACQUIRED, old,
                               PRESENCE_MAYBE_SOMEONE);
        }
        break;

    /* ── MAYBE_SOMEONE ──────────────────────────────────────────────────── */
    case PRESENCE_MAYBE_SOMEONE:
        if (!signal) {
            s_maybe_elapsed_ms += dt_ms;
            if (s_maybe_elapsed_ms >= MAYBE_TIMEOUT_MS) {
                ESP_LOGD(TAG, "MAYBE timeout → NO_ONE");
                s_state            = PRESENCE_NO_ONE;
                s_maybe_elapsed_ms = 0U;
                s_heur_conf_ms     = 0U;
            }
            break;
        }
        s_maybe_elapsed_ms = 0U;

        if (face_valid) {
            /* Face box: confirmação imediata */
            s_state                = PRESENCE_PRESENT;
            s_confidence_confirmed = true;
            s_engaged_ms           = 0U;
            s_hold_ms              = 0U;
            s_confirmed_count++;
            publish_transition(NB_EVT_SOCIAL_PRESENCE_CONFIRMED,
                               PRESENCE_MAYBE_SOMEONE, PRESENCE_PRESENT);
        } else {
            /* Só heurística: aguarda HEURISTIC_CONFIRM_MS sustentado */
            s_heur_conf_ms += dt_ms;
            if (s_heur_conf_ms >= HEURISTIC_CONFIRM_MS) {
                s_state                = PRESENCE_PRESENT;
                s_confidence_confirmed = false;
                s_engaged_ms           = 0U;
                s_hold_ms              = 0U;
                s_confirmed_count++;
                publish_transition(NB_EVT_SOCIAL_PRESENCE_CONFIRMED,
                                   PRESENCE_MAYBE_SOMEONE, PRESENCE_PRESENT);
            }
        }
        break;

    /* ── PRESENT ────────────────────────────────────────────────────────── */
    case PRESENCE_PRESENT:
        s_engaged_ms += dt_ms;
        if (signal) {
            s_hold_ms = 0U;
            if (face_valid) s_confidence_confirmed = true;
        } else {
            s_hold_ms += dt_ms;
            if (s_hold_ms >= HOLD_MS) {
                s_state         = PRESENCE_LEFT_RECENTLY;
                s_since_left_ms = 0U;
                s_engaged_ms    = 0U;
                s_hold_ms       = 0U;
                publish_transition(NB_EVT_SOCIAL_PRESENCE_LOST_SHORT,
                                   PRESENCE_PRESENT, PRESENCE_LEFT_RECENTLY);
                break;
            }
        }
        /* Verifica transição para ENGAGED somente se ainda em PRESENT */
        if (s_state == PRESENCE_PRESENT && s_engaged_ms >= ENGAGED_MS) {
            s_state = PRESENCE_ENGAGED;
            publish_transition(NB_EVT_SOCIAL_PRESENCE_ENGAGED,
                               PRESENCE_PRESENT, PRESENCE_ENGAGED);
        }
        break;

    /* ── ENGAGED ────────────────────────────────────────────────────────── */
    case PRESENCE_ENGAGED:
        if (signal) {
            s_hold_ms = 0U;
            if (face_valid) s_confidence_confirmed = true;
        } else {
            s_hold_ms += dt_ms;
            if (s_hold_ms >= HOLD_MS) {
                s_state         = PRESENCE_LEFT_RECENTLY;
                s_since_left_ms = 0U;
                s_hold_ms       = 0U;
                publish_transition(NB_EVT_SOCIAL_PRESENCE_LOST_SHORT,
                                   PRESENCE_ENGAGED, PRESENCE_LEFT_RECENTLY);
            }
        }
        break;

    /* ── LEFT_RECENTLY ──────────────────────────────────────────────────── */
    case PRESENCE_LEFT_RECENTLY:
        s_since_left_ms += dt_ms;
        if (signal) {
            /* Retorno antes de 30s: CONFIRMED (ausência muito curta para RETURNED) */
            presence_state_t old = PRESENCE_LEFT_RECENTLY;
            s_state              = PRESENCE_PRESENT;
            s_hold_ms            = 0U;
            s_engaged_ms         = 0U;
            if (face_valid) s_confidence_confirmed = true;
            s_confirmed_count++;
            publish_transition(NB_EVT_SOCIAL_PRESENCE_CONFIRMED, old, PRESENCE_PRESENT);
            break;
        }
        if (s_since_left_ms >= LEFT_RECENTLY_MS) {
            s_state = PRESENCE_AWAY;
            s_away_count++;
            publish_transition(NB_EVT_SOCIAL_PRESENCE_AWAY,
                               PRESENCE_LEFT_RECENTLY, PRESENCE_AWAY);
        }
        break;

    /* ── AWAY ───────────────────────────────────────────────────────────── */
    case PRESENCE_AWAY:
        s_since_left_ms += dt_ms;
        if (signal) {
            /* RETURNED se ausência total ≥ 60s; CONFIRMED caso contrário */
            presence_state_t old = PRESENCE_AWAY;
            s_state              = PRESENCE_PRESENT;
            s_hold_ms            = 0U;
            s_engaged_ms         = 0U;
            if (face_valid) s_confidence_confirmed = true;
            if (s_since_left_ms >= RETURNED_MIN_MS) {
                s_returned_count++;
                publish_transition(NB_EVT_SOCIAL_PRESENCE_RETURNED, old, PRESENCE_PRESENT);
            } else {
                s_confirmed_count++;
                publish_transition(NB_EVT_SOCIAL_PRESENCE_CONFIRMED, old, PRESENCE_PRESENT);
            }
            break;
        }
        /* s_since_left_ms inclui tempo em LEFT_RECENTLY + AWAY */
        if (s_since_left_ms >= LEFT_RECENTLY_MS + AWAY_MS) {
            s_state = PRESENCE_ALONE_SETTLED;
            publish_transition(NB_EVT_SOCIAL_PRESENCE_SETTLED,
                               PRESENCE_AWAY, PRESENCE_ALONE_SETTLED);
        }
        break;

    default:
        break;
    }
}

/* ── API ─────────────────────────────────────────────────────────────────── */

esp_err_t presence_semantic_init(void)
{
    esp_err_t err;

    err = nb_event_subscribe(NB_EVT_BRIDGE_FACE_BOX, on_face_box, NULL, NULL);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "subscribe FACE_BOX falhou: %s", esp_err_to_name(err));

    err = nb_event_subscribe(NB_EVT_PRESENCE_DETECTED, on_presence_detected, NULL, NULL);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "subscribe PRESENCE_DETECTED falhou: %s", esp_err_to_name(err));

    err = nb_event_subscribe(NB_EVT_PRESENCE_LOST, on_presence_lost, NULL, NULL);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "subscribe PRESENCE_LOST falhou: %s", esp_err_to_name(err));

    /* BRIDGE_CONNECTED não requer ação: face_box chegará naturalmente. */
    err = nb_event_subscribe(NB_EVT_BRIDGE_DISCONNECTED, on_bridge_disconnected, NULL, NULL);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "subscribe BRIDGE_DISCONNECTED falhou: %s", esp_err_to_name(err));

    ESP_LOGI(TAG, "presence_semantic_service inicializado");
    return ESP_OK;
}

presence_diag_t presence_semantic_get_diag(void)
{
    return (presence_diag_t){
        .state                    = s_state,
        .presence_session_s       = s_presence_session_ms / 1000U,
        .absence_s                = s_absence_ms / 1000U,
        .last_seen_at_ms          = s_last_seen_at_ms,
        .confidence_confirmed     = s_confidence_confirmed,
        .presence_confirmed_count = s_confirmed_count,
        .presence_returned_count  = s_returned_count,
        .presence_away_count      = s_away_count,
    };
}
