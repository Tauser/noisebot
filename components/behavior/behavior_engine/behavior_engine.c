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

#include "state_machine.h"
#include "emotion_model.h"
#include "conductor.h"
#include "idle_service.h"
#include "long_term_memory.h"

#include "freertos/FreeRTOS.h"
#include "esp_log.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define TAG "nb_beng"

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

static bool cond_waking(const nb_event_t *evt)
{
    return STATE_NEW(evt) == NB_STATE_IDLE &&
           STATE_OLD(evt) == NB_STATE_SLEEPING;
}

static bool cond_error(const nb_event_t *evt)
{
    return STATE_NEW(evt) == NB_STATE_ERROR;
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

    { NB_EVT_STATE_CHANGED, cond_error, {
        ACT_EMOT(MOTION_FAULT) }},

    /* ── Solidão (idle alone) ───────────────────────────────────────────────── */
    { NB_EVT_IDLE_ALONE, NULL, {
        ACT_EMOT(IDLE_LONG) }},

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

static bool s_initialized = false;

/* ── Helpers de execução ─────────────────────────────────────────────────── */

static void execute_action(const nb_be_action_t *act)
{
    switch (act->type) {
        case NB_BE_ACT_EMIT_EMOTION:
            emotion_model_on_event(act->arg.emotion);
            break;
        case NB_BE_ACT_PLAY_CONDUCTOR:
            conductor_play(act->arg.conductor);
            break;
        case NB_BE_ACT_LTM_RECORD:
            ltm_record(act->arg.ltm);
            break;
        case NB_BE_ACT_LTM_FLUSH:
            ltm_flush();
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

    s_initialized = true;
    NB_LOGI(TAG, "behavior_engine inicializado (%u regras)", (unsigned)K_NRULES);
    return ESP_OK;
}
