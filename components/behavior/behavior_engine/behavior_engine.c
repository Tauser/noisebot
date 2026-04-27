/*
 * behavior_engine.c — Motor de regras comportamentais (Layer 6)
 *
 * Tabela declarativa: cada regra mapeia (trigger, condition) → actions[].
 * Avaliada em cada evento publicado no bus via subscrições registradas no init.
 *
 * Prioridade de dispatch:
 *   1. Regras COM condition são avaliadas primeiro (em ordem de declaração).
 *      Todas que satisfazem a condition são executadas (conditions não se
 *      sobrepõem na tabela atual — ex: sleeping / waking / error são mutuamente
 *      exclusivos para NB_EVT_STATE_CHANGED).
 *   2. Regras SEM condition (cond = NULL) só são executadas se NENHUMA regra
 *      com condition tiver disparado para aquele trigger.
 *
 * Encoding do payload de NB_EVT_STATE_CHANGED:
 *   bits [7:0]  — new_state (nb_robot_state_t)
 *   bits [15:8] — old_state (nb_robot_state_t)
 *   (boot_manager.c empacota assim desde a Etapa 9.3)
 */

#include "behavior_engine.h"

#include "event_bus.h"
#include "nb_events.h"
#include "logger.h"
#include "bridge_service.h"

#include "state_machine.h"
#include "emotion_model.h"
#include "conductor.h"
#include "idle_service.h"
#include "long_term_memory.h"
#include "persona_service.h"
#include "expression_service.h"
#include "gaze_service.h"
#include "ui_overlay_service.h"
#include "led_service.h"
#include "audio_service.h"
#include "synth_service.h"
#include "attention_service.h"
#include "diagnostics_service.h"

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define TAG "nb_beng"

#define BRIDGE_ERROR_TOAST_MS   2400U

/* ── Tipos internos ──────────────────────────────────────────────────────── */

/** Número máximo de ações por regra. */
#define NB_BE_MAX_ACTIONS  6U

typedef enum {
    NB_BE_ACT_NONE = 0,
    NB_BE_ACT_EMIT_EMOTION,      /**< emotion_model_on_event(arg.emotion)   */
    NB_BE_ACT_PLAY_CONDUCTOR,    /**< conductor_play(arg.conductor)          */
    NB_BE_ACT_LTM_RECORD,        /**< ltm_record(arg.ltm)                    */
    NB_BE_ACT_LTM_FLUSH,         /**< ltm_flush()                            */
    NB_BE_ACT_IDLE_INTERACT,     /**< idle_service_on_interaction()           */
} nb_be_act_type_t;

typedef struct {
    nb_be_act_type_t type;
    union {
        nb_emotion_event_t  emotion;
        nb_action_t         conductor;
        ltm_iact_type_t     ltm;
    } arg;
} nb_be_action_t;

/** Condition: recebe o evento e retorna true se a regra deve disparar. */
typedef bool (*nb_be_cond_fn_t)(const nb_event_t *evt);

typedef struct {
    nb_event_type_t  trigger;
    nb_be_cond_fn_t  cond;                         /**< NULL = sempre dispara */
    nb_be_action_t   actions[NB_BE_MAX_ACTIONS];   /**< Terminado por NONE    */
} nb_be_rule_t;

/* ── Helpers de construção de regras ─────────────────────────────────────── */

#define ACT_EMOT(e) { .type = NB_BE_ACT_EMIT_EMOTION,   .arg = { .emotion   = NB_EMOT_EVT_##e } }
#define ACT_PLAY(a) { .type = NB_BE_ACT_PLAY_CONDUCTOR, .arg = { .conductor = NB_ACTION_##a   } }
#define ACT_LTM(l)  { .type = NB_BE_ACT_LTM_RECORD,     .arg = { .ltm       = LTM_IACT_##l   } }
#define ACT_FLUSH   { .type = NB_BE_ACT_LTM_FLUSH,      .arg = { .emotion   = 0              } }
#define ACT_IDLE    { .type = NB_BE_ACT_IDLE_INTERACT,  .arg = { .emotion   = 0              } }

/* ── Condition functions ─────────────────────────────────────────────────── */

/* Extrai new_state e old_state do payload empacotado pelo boot_manager. */
#define STATE_NEW(evt)  ((nb_robot_state_t)((evt)->data.u32 & 0xFFU))
#define STATE_OLD(evt)  ((nb_robot_state_t)(((evt)->data.u32 >> 8) & 0xFFU))

static bool cond_sleeping(const nb_event_t *evt)
{
    return STATE_NEW(evt) == NB_STATE_SLEEPING;
}

/* Waking normal (warmth ≤ 0.7): saudação discreta. */
static bool cond_waking(const nb_event_t *evt)
{
    return STATE_NEW(evt) == NB_STATE_IDLE &&
           STATE_OLD(evt) == NB_STATE_SLEEPING &&
           persona_get_warmth() <= 0.7f;
}

/* Waking com alta familiaridade (warmth > 0.7): saudação entusiasmada. */
static bool cond_waking_warm(const nb_event_t *evt)
{
    return STATE_NEW(evt) == NB_STATE_IDLE &&
           STATE_OLD(evt) == NB_STATE_SLEEPING &&
           persona_get_warmth() > 0.7f;
}

static bool cond_error(const nb_event_t *evt)
{
    return STATE_NEW(evt) == NB_STATE_ERROR;
}

static bool cond_meditation(const nb_event_t *evt)
{
    return STATE_NEW(evt) == NB_STATE_MEDITATION;
}

static bool cond_meditation_exit(const nb_event_t *evt)
{
    return STATE_NEW(evt) == NB_STATE_IDLE &&
           STATE_OLD(evt) == NB_STATE_MEDITATION;
}

static bool cond_silent_company(const nb_event_t *evt)
{
    return STATE_NEW(evt) == NB_STATE_SILENT_COMPANY;
}

static bool cond_silent_company_exit(const nb_event_t *evt)
{
    return STATE_NEW(evt) == NB_STATE_IDLE &&
           STATE_OLD(evt) == NB_STATE_SILENT_COMPANY;
}

/* Persona: baixa confiança — reações defensivas. */
static bool cond_trust_low(const nb_event_t *evt)
{
    (void)evt;
    return persona_get_trust() < 0.3f;
}

/* ── Tabela de regras ────────────────────────────────────────────────────── */
/*
 * Campos não declarados são zero-inicializados (actions[N].type = NB_BE_ACT_NONE).
 * A execução para no primeiro slot com type = NONE.
 */
static const nb_be_rule_t k_rules[] = {

    /* ── Touch ──────────────────────────────────────────────────────────── */
    /* SM inputs (tap/long/wake) são chamados diretamente no on_touch_event  */
    /* do boot_manager, antes de publicar o evento. Aqui só comportamento.  */

    /* Persona: trust < 0.3 → susto; trust >= 0.3 → calor (unconditional) */
    { NB_EVT_TOUCH_TAP, cond_trust_low, {
        ACT_EMOT(TOUCH_TAP), ACT_IDLE,
        ACT_PLAY(TOUCH_STARTLE), ACT_LTM(TOUCH_TAP) }},

    { NB_EVT_TOUCH_TAP, NULL, {
        ACT_EMOT(TOUCH_TAP), ACT_IDLE,
        ACT_PLAY(TOUCH_WARM), ACT_LTM(TOUCH_TAP) }},

    { NB_EVT_TOUCH_LONG_PRESS, NULL, {
        ACT_EMOT(TOUCH_LONG),
        ACT_PLAY(TOUCH_STARTLE), ACT_LTM(TOUCH_LONG) }},

    { NB_EVT_TOUCH_WAKE, NULL, {
        ACT_PLAY(WAKE_UP), ACT_LTM(WAKE) }},

    /* ── Voice / Áudio ───────────────────────────────────────────────────── */
    /* SM inputs (voice_start/end, audio_started/ended) chamados no callback */
    /* do audio_service (on_audio_event), antes de publicar. Só comportamento. */

    /* Persona: trust < 0.3 → voz desconhecida dispara ALARMED (não CURIOUS) */
    { NB_EVT_VOICE_ACTIVITY_START, cond_trust_low, {
        ACT_EMOT(VOICE_LOUD), ACT_IDLE, ACT_LTM(VOICE_START) }},

    { NB_EVT_VOICE_ACTIVITY_START, NULL, {
        ACT_EMOT(VOICE_START), ACT_IDLE,
        ACT_PLAY(CURIOUS), ACT_LTM(VOICE_START) }},

    { NB_EVT_AUDIO_STARTED, NULL, {
        ACT_EMOT(AUDIO_STARTED),
        ACT_PLAY(SPEAK_LOOP), ACT_LTM(AUDIO_PLAYED) }},

    /* ── State transitions (condicionadas — mutuamente exclusivas) ────────── */
    { NB_EVT_STATE_CHANGED, cond_sleeping, {
        ACT_EMOT(ENTERING_SLEEP), ACT_PLAY(SLEEP), ACT_LTM(SLEEP), ACT_FLUSH }},

    { NB_EVT_STATE_CHANGED, cond_waking, {
        ACT_EMOT(WAKING_UP), ACT_PLAY(WAKE_UP), ACT_LTM(WAKE) }},

    /* Persona: warmth > 0.7 → saudação entusiasmada ao acordar */
    { NB_EVT_STATE_CHANGED, cond_waking_warm, {
        ACT_EMOT(WAKING_UP), ACT_PLAY(GREET), ACT_LTM(WAKE) }},

    { NB_EVT_STATE_CHANGED, cond_error, {
        ACT_EMOT(MOTION_FAULT) }},

    { NB_EVT_STATE_CHANGED, cond_meditation, {
        ACT_EMOT(ENTERING_SLEEP) }},

    { NB_EVT_STATE_CHANGED, cond_meditation_exit, {
        ACT_EMOT(WAKING_UP) }},

    { NB_EVT_STATE_CHANGED, cond_silent_company, {
        ACT_EMOT(ENTERING_SLEEP) }},

    { NB_EVT_STATE_CHANGED, cond_silent_company_exit, {
        ACT_EMOT(WAKING_UP) }},

    /* ── Marcos de uso (Etapa 11.4) ─────────────────────────────────────────── */
    { NB_EVT_MILESTONE_TOUCH_50,    NULL, { ACT_PLAY(CELEBRATE) }},
    { NB_EVT_MILESTONE_UPTIME_100H, NULL, { ACT_PLAY(CELEBRATE) }},

    /* ── Solidão (idle alone) ───────────────────────────────────────────────── */
    { NB_EVT_IDLE_ALONE, NULL, {
        ACT_EMOT(IDLE_LONG) }},

    /* ── Touch Semântico (Etapa 10.4) ───────────────────────────────────────── */
    { NB_EVT_TOUCH_DOUBLE_TAP, NULL, {
        ACT_EMOT(TOUCH_TAP), ACT_PLAY(TOUCH_WARM),
        ACT_LTM(TOUCH_DOUBLE_TAP) }},                       /* alegria breve    */

    { NB_EVT_TOUCH_WARM_PULSE, NULL, {
        ACT_EMOT(TOUCH_WARM_PULSE) }},                      /* calor acumulando */

    { NB_EVT_TOUCH_DEEP, NULL, {
        ACT_EMOT(TOUCH_DEEP), ACT_LTM(TOUCH_DEEP) }},      /* calor intenso    */

    { NB_EVT_TOUCH_CARESS, NULL, {
        ACT_EMOT(TOUCH_CARESS), ACT_LTM(TOUCH_CARESS) }},  /* satisfação       */

    /* ── VAD Semântico (Etapa 10.3) ─────────────────────────────────────────── */
    { NB_EVT_VOICE_LONG, NULL, {
        ACT_PLAY(AGREE) }},                          /* nod de concordância       */

    { NB_EVT_VOICE_LOUD, NULL, {
        ACT_EMOT(VOICE_LOUD) }},                     /* alarme/surpresa momentâneo */

    { NB_EVT_VOICE_SOFT, NULL, {
        ACT_EMOT(VOICE_SOFT), ACT_PLAY(CURIOUS) }}, /* curiosidade + inclinação   */

    { NB_EVT_VOICE_REPEATED, NULL, {
        ACT_PLAY(CURIOUS) }},                        /* "não entendi, pode repetir?" */
};

#define K_NRULES  ((uint8_t)(sizeof(k_rules) / sizeof(k_rules[0])))

/* ── Estado ──────────────────────────────────────────────────────────────── */

static bool s_initialized        = false;
static bool s_touch_50_milestone = false;

/* ── Helpers de execução ─────────────────────────────────────────────────── */

static void apply_expression_overlay_for_emotion(nb_emotion_event_t event)
{
    switch (event) {
        case NB_EMOT_EVT_TOUCH_TAP:
            expression_service_overlay_blush(42U, 900U);
            break;
        case NB_EMOT_EVT_TOUCH_WARM_PULSE:
            expression_service_overlay_blush(80U, 1800U);
            break;
        case NB_EMOT_EVT_TOUCH_DEEP:
            expression_service_overlay_blush(150U, 4200U);
            break;
        case NB_EMOT_EVT_TOUCH_CARESS:
            expression_service_overlay_blush(230U, 5200U);
            expression_service_overlay_heart(1800U);
            break;
        default:
            break;
    }
}

static void apply_touch_feedback_for_event(nb_event_type_t event)
{
    switch (event) {
        case NB_EVT_TOUCH_DOUBLE_TAP:
            led_effect_heartbeat();
            expression_service_overlay_heart(1200U);
            synth_blip(740.0f, 70U);
            break;
        case NB_EVT_TOUCH_LONG_PRESS:
            led_blink(1U);
            break;
        case NB_EVT_TOUCH_WARM_PULSE:
            led_effect_touch();
            break;
        case NB_EVT_TOUCH_DEEP:
            led_effect_heartbeat();
            break;
        case NB_EVT_TOUCH_CARESS:
            led_effect_heartbeat();
            expression_service_overlay_heart(2400U);
            break;
        default:
            break;
    }
}

static void execute_action(const nb_be_action_t *act)
{
    switch (act->type) {
        case NB_BE_ACT_EMIT_EMOTION:
            emotion_model_on_event(act->arg.emotion);
            apply_expression_overlay_for_emotion(act->arg.emotion);
            break;
        case NB_BE_ACT_PLAY_CONDUCTOR:
            conductor_play(act->arg.conductor);
            break;
        case NB_BE_ACT_LTM_RECORD:
            ltm_record(act->arg.ltm);
            if (!s_touch_50_milestone &&
                (act->arg.ltm == LTM_IACT_TOUCH_TAP        ||
                 act->arg.ltm == LTM_IACT_TOUCH_LONG       ||
                 act->arg.ltm == LTM_IACT_TOUCH_DOUBLE_TAP ||
                 act->arg.ltm == LTM_IACT_TOUCH_DEEP       ||
                 act->arg.ltm == LTM_IACT_TOUCH_CARESS) &&
                ltm_get_total_touch_count() >= 50U) {
                s_touch_50_milestone = true;
                nb_event_t ms = { .type = NB_EVT_MILESTONE_TOUCH_50 };
                nb_event_publish_async(&ms);
            }
            break;
        case NB_BE_ACT_LTM_FLUSH:
            ltm_flush();
            persona_service_refresh();
            idle_service_set_saccade_multiplier(
                persona_get_curiosity() > 0.6f ? 1.5f : 1.0f);
            break;
        case NB_BE_ACT_IDLE_INTERACT:
            idle_service_on_interaction();
            break;
        case NB_BE_ACT_NONE:
        default:
            break;
    }
}

static void execute_rule(const nb_be_rule_t *rule)
{
    for (uint8_t i = 0; i < NB_BE_MAX_ACTIONS; i++) {
        if (rule->actions[i].type == NB_BE_ACT_NONE) break;
        execute_action(&rule->actions[i]);
    }
}

/* ── Bridge — mapeamento de ação ─────────────────────────────────────────── */

static const nb_action_t k_bridge_action_map[] = {
    [NB_BRIDGE_ACTION_GREET]      = NB_ACTION_GREET,
    [NB_BRIDGE_ACTION_NOD]        = NB_ACTION_AGREE,
    [NB_BRIDGE_ACTION_SHAKE]      = NB_ACTION_DISAGREE,
    [NB_BRIDGE_ACTION_LOOK_UP]    = NB_ACTION_CURIOUS,
    [NB_BRIDGE_ACTION_LOOK_DOWN]  = NB_ACTION_CURIOUS,
};

/* ── Bridge — timer de resposta (8s após VOICE_END) ─────────────────────── */

static esp_timer_handle_t s_bridge_resp_timer;
static bool               s_bridge_say_started;
static bool               s_bridge_voice_pending;

static void bridge_resp_timeout_cb(void *arg)
{
    (void)arg;
    nb_event_t evt = {
        .type         = NB_EVT_BRIDGE_RESPONSE_TIMEOUT,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
    };
    nb_event_publish_async(&evt);
}

static void bridge_on_event(const nb_event_t *evt)
{
    if (evt->type == NB_EVT_WAKE_WORD_DETECTED) {
        ui_overlay_show_toast("Ouvindo...", NB_UI_OVERLAY_INFO, 2200U);
    }

    switch (evt->type) {

    case NB_EVT_WAKE_WORD_DETECTED:
        s_bridge_say_started = false;
        s_bridge_voice_pending = false;
        esp_timer_stop(s_bridge_resp_timer);
        break;

    case NB_EVT_VOICE_ACTIVITY_START:
        s_bridge_say_started = false;
        s_bridge_voice_pending = true;
        esp_timer_stop(s_bridge_resp_timer);
        break;

    case NB_EVT_VOICE_ACTIVITY_END:
        if (bridge_service_is_connected() && s_bridge_voice_pending) {
            s_bridge_say_started = false;
            s_bridge_voice_pending = false;
            esp_timer_stop(s_bridge_resp_timer);
            esp_timer_start_once(s_bridge_resp_timer, 8000000LL);
        }
        break;

    case NB_EVT_BRIDGE_SAY: {
        if (!s_bridge_say_started) {
            s_bridge_say_started = true;
            esp_timer_stop(s_bridge_resp_timer);
        }
        const nb_bridge_say_chunk_t *chunk = (const nb_bridge_say_chunk_t *)evt->data.ptr;
        if (chunk) {
            audio_service_bridge_say_chunk(chunk->samples, chunk->count);
        }
        break;
    }

    case NB_EVT_BRIDGE_EXPR: {
        const nb_bridge_expr_cmd_t *cmd = (const nb_bridge_expr_cmd_t *)evt->data.ptr;
        if (cmd && cmd->expression_id < NB_EXPR_COUNT) {
            expression_service_set((nb_expression_t)cmd->expression_id,
                                   (float)cmd->duration_ms);
        }
        break;
    }

    case NB_EVT_BRIDGE_ACTION: {
        nb_bridge_action_t ba = (nb_bridge_action_t)evt->data.u32;
        if (ba < NB_BRIDGE_ACTION_COUNT) {
            conductor_play(k_bridge_action_map[ba]);
        }
        break;
    }

    case NB_EVT_BRIDGE_EMOT_EVENT:
        if (evt->data.u32 < NB_EMOT_EVT_COUNT) {
            nb_emotion_event_t event = (nb_emotion_event_t)evt->data.u32;
            emotion_model_on_event(event);
            apply_expression_overlay_for_emotion(event);
        }
        break;

    case NB_EVT_BRIDGE_GAZE: {
        float x, y;
        memcpy(&x, &evt->data.bytes[0], sizeof(float));
        memcpy(&y, &evt->data.bytes[4], sizeof(float));
        gaze_service_set_target(x, y);
        break;
    }

    case NB_EVT_BRIDGE_VOLUME: {
        uint8_t level = (evt->data.u32 > 100U) ? 100U : (uint8_t)evt->data.u32;
        audio_set_volume(level);
        synth_set_volume(level);
        synth_blip(880.0f, 90U);
        ui_overlay_show_volume(level, 1800U);
        NB_LOGI(TAG, "volume via bridge: %u%%", (unsigned)level);
        break;
    }

    case NB_EVT_BRIDGE_TEXT_SCROLL: {
        const char *text = (const char *)evt->data.ptr;
        if (text) {
            ui_overlay_show_text(text, 1800U);
            NB_LOGI(TAG, "texto via bridge: %s", text);
        }
        break;
    }

    case NB_EVT_BRIDGE_SESSION: {
        const char *payload = (const char *)evt->data.ptr;
        if (!payload) break;

        if (strstr(payload, "\"event\":\"LISTEN_START\"")) {
            ui_overlay_show_toast("Ouvindo...", NB_UI_OVERLAY_INFO, 2200U);
        } else if (strstr(payload, "\"event\":\"TRANSCRIBE_START\"")) {
            ui_overlay_show_toast("Transcrevendo...", NB_UI_OVERLAY_INFO, 2200U);
        } else if (strstr(payload, "\"event\":\"THINKING_START\"")) {
            ui_overlay_show_toast("Pensando...", NB_UI_OVERLAY_INFO, 2200U);
        } else if (strstr(payload, "\"event\":\"TTS_START\"")) {
            ui_overlay_show_toast("Falando...", NB_UI_OVERLAY_SUCCESS, 2200U);
        } else if (strstr(payload, "\"event\":\"SESSION_ERROR\"")) {
            if (strstr(payload, "\"reason\":\"llm_quota_exceeded\"")) {
                ui_overlay_show_toast("Cota da LLM", NB_UI_OVERLAY_ERROR, BRIDGE_ERROR_TOAST_MS);
            } else if (strstr(payload, "\"reason\":\"llm_unavailable\"") ||
                       strstr(payload, "\"reason\":\"llm_error\"")) {
                ui_overlay_show_toast("Erro na LLM", NB_UI_OVERLAY_ERROR, BRIDGE_ERROR_TOAST_MS);
            } else if (strstr(payload, "\"reason\":\"stt_rejected\"")) {
                ui_overlay_show_toast("Nao entendi", NB_UI_OVERLAY_WARNING, BRIDGE_ERROR_TOAST_MS);
            } else if (strstr(payload, "\"reason\":\"audio_rejected\"")) {
                ui_overlay_show_toast("Nao ouvi", NB_UI_OVERLAY_WARNING, BRIDGE_ERROR_TOAST_MS);
            } else {
                ui_overlay_show_toast("Erro na conversa", NB_UI_OVERLAY_ERROR, BRIDGE_ERROR_TOAST_MS);
            }
        }
        break;
    }

    case NB_EVT_BRIDGE_DISCONNECTED:
        s_bridge_voice_pending = false;
        esp_timer_stop(s_bridge_resp_timer);
        expression_service_set(NB_EXPR_NEUTRAL, 500.0f);
        ui_overlay_show_toast("Bridge offline", NB_UI_OVERLAY_WARNING, BRIDGE_ERROR_TOAST_MS);
        break;

    case NB_EVT_BRIDGE_RESPONSE_TIMEOUT:
        s_bridge_voice_pending = false;
        expression_service_set(NB_EXPR_NEUTRAL, 500.0f);
        ui_overlay_show_toast("Sem resposta", NB_UI_OVERLAY_WARNING, BRIDGE_ERROR_TOAST_MS);
        NB_LOGW(TAG, "bridge sem resposta em 8s — retornando a idle");
        break;

    case NB_EVT_STATE_CHANGED:
    case NB_EVT_WIFI_IP_ACQUIRED: {
        /* Atualiza status no bridge sempre que o estado do sistema muda */
        nb_bridge_status_t st = {
            .state        = (uint8_t)state_machine_get_state(),
            .valence      = emotion_model_get_vec().valence,
            .activation   = emotion_model_get_vec().activation,
            .attention    = attention_service_get_level(),
            .health_score = diagnostics_get_health_score(),
        };
        bridge_service_update_status(&st);
        break;
    }

    default:
        break;
    }
}

/* ── Handler do event bus ────────────────────────────────────────────────── */

static void on_bus_event(const nb_event_t *evt, void *ctx)
{
    (void)ctx;

    /* Passa 1: executa regras com condition que satisfazem a condição. */
    bool any_cond_fired = false;
    for (uint8_t i = 0; i < K_NRULES; i++) {
        if (k_rules[i].trigger != evt->type) continue;
        if (k_rules[i].cond == NULL)         continue;
        if (!k_rules[i].cond(evt))           continue;
        execute_rule(&k_rules[i]);
        any_cond_fired = true;
    }

    /* Passa 2: executa regras fallback (cond = NULL) se nenhuma condicionada
     * disparou para este trigger. */
    if (!any_cond_fired) {
        for (uint8_t i = 0; i < K_NRULES; i++) {
            if (k_rules[i].trigger != evt->type) continue;
            if (k_rules[i].cond != NULL)         continue;
            execute_rule(&k_rules[i]);
        }
    }

    apply_touch_feedback_for_event(evt->type);

    /* Passa 3: wiring do bridge (Etapa 12.2) — independente das regras. */
    bridge_on_event(evt);
}

/* ── API ─────────────────────────────────────────────────────────────────── */

esp_err_t behavior_engine_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;

    /*
     * Coleta triggers únicos da tabela e subscreve uma vez por tipo.
     * Usa array local de flags indexado por nb_event_type_t.
     * NB_EVT_COUNT é pequeno o suficiente para stack (≤ 64 tipos).
     */
    bool subscribed[NB_EVT_COUNT];
    memset(subscribed, 0, sizeof(subscribed));

    for (uint8_t i = 0; i < K_NRULES; i++) {
        nb_event_type_t t = k_rules[i].trigger;
        if (subscribed[t]) continue;

        esp_err_t err = nb_event_subscribe(t, on_bus_event, NULL, NULL);
        if (err != ESP_OK) {
            NB_LOGW(TAG, "subscribe(%d) falhou: %s", (int)t, esp_err_to_name(err));
        }
        subscribed[t] = true;
    }

    /* Subscreve eventos de bridge que não estão na tabela de regras */
    static const nb_event_type_t k_bridge_evts[] = {
        NB_EVT_WAKE_WORD_DETECTED,
        NB_EVT_VOICE_ACTIVITY_END,
        NB_EVT_BRIDGE_SAY,
        NB_EVT_BRIDGE_EXPR,
        NB_EVT_BRIDGE_ACTION,
        NB_EVT_BRIDGE_EMOT_EVENT,
        NB_EVT_BRIDGE_GAZE,
        NB_EVT_BRIDGE_TEXT_SCROLL,
        NB_EVT_BRIDGE_VOLUME,
        NB_EVT_BRIDGE_SESSION,
        NB_EVT_BRIDGE_DISCONNECTED,
        NB_EVT_BRIDGE_RESPONSE_TIMEOUT,
        NB_EVT_STATE_CHANGED,
        NB_EVT_WIFI_IP_ACQUIRED,
    };
    for (size_t i = 0; i < sizeof(k_bridge_evts) / sizeof(k_bridge_evts[0]); i++) {
        nb_event_type_t t = k_bridge_evts[i];
        if (!subscribed[t]) {
            nb_event_subscribe(t, on_bus_event, NULL, NULL);
            subscribed[t] = true;
        }
    }

    /* Timer one-shot para timeout de resposta do bridge (8s) */
    const esp_timer_create_args_t timer_args = {
        .callback = bridge_resp_timeout_cb,
        .name     = "bridge_resp",
    };
    esp_timer_create(&timer_args, &s_bridge_resp_timer);

    s_initialized = true;
    NB_LOGI(TAG, "behavior_engine inicializado (%u regras)", (unsigned)K_NRULES);
    return ESP_OK;
}
