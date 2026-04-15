/*
 * idle_service.c — Microbehaviors de idle do NoiseBot (Layer 5)
 *
 * Todos os timers são aleatórios (intervalos min+rand*range) para evitar
 * periodicidade mecânica. O hardware RNG do ESP32 garante entropia real.
 *
 * Comportamento por estado:
 *   IDLE || ATTENTIVE:  micro-saccade a cada 5–15s.
 *   ATTENTIVE somente:  aversive gaze a cada 8–15s.
 *   IDLE somente:       yawn a cada 60–180s.
 *   Outros estados:     timers resetados, gaze retorna a center (0, 0).
 *
 * Nota: micro-neck-movement (<5°, ≤3/min) requer motion_service (Etapa 3.3).
 * Stub preparado — ativado quando motion for liberado.
 */

#include "idle_service.h"
#include "gaze_service.h"
#include "expression_service.h"
#include "state_machine.h"

#include "esp_log.h"
#include "esp_random.h"

#define TAG "nb_idle"

/* ── Parâmetros de timing ────────────────────────────────────────────────── */

#define SACCADE_MIN_MS      5000U    /* intervalo mínimo de micro-saccade     */
#define SACCADE_RANGE_MS   10000U    /* variação adicional (sorteada)         */

#define AVERSIVE_MIN_MS     8000U    /* intervalo mínimo de aversive gaze     */
#define AVERSIVE_RANGE_MS   7000U

#define YAWN_MIN_MS        60000U    /* intervalo mínimo de yawn              */
#define YAWN_RANGE_MS     120000U
#define YAWN_DURATION_MS    2500U    /* tempo em SLEEPY antes de retornar     */
#define YAWN_TRANS_IN_MS     800.0f  /* transição para SLEEPY                 */
#define YAWN_TRANS_OUT_MS    600.0f  /* transição de volta para NEUTRAL       */

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Número aleatório em [0, 1) usando hardware RNG */
static inline float rand01(void)
{
    return (float)(esp_random() >> 1) / 2147483648.0f;
}

/* Número aleatório em [-1, 1] usando hardware RNG */
static inline float rand11(void)
{
    return (float)(int32_t)esp_random() / 2147483648.0f;
}

static inline uint32_t rand_interval(uint32_t min_ms, uint32_t range_ms)
{
    return min_ms + (uint32_t)((float)range_ms * rand01());
}

/* ── Estado interno ──────────────────────────────────────────────────────── */

static bool     s_initialized       = false;

static uint32_t s_saccade_timer_ms  = 0;
static uint32_t s_aversive_timer_ms = 0;
static uint32_t s_yawn_timer_ms     = 0;
static uint32_t s_yawn_hold_ms      = 0;   /* > 0 = yawn em curso     */

static bool     s_was_active        = false; /* estava em IDLE/ATTENTIVE */

/* ── Behaviors ───────────────────────────────────────────────────────────── */

static void do_micro_saccade(void)
{
    /* 20% de chance de voltar ao centro — aumenta naturalidade */
    float x, y;
    if (rand01() < 0.20f) {
        x = 0.0f;
        y = 0.0f;
    } else {
        x = rand11() * 0.50f;
        y = rand11() * 0.30f;
    }
    gaze_service_set_target(x, y);
    ESP_LOGD(TAG, "micro-saccade → (%.2f, %.2f)", x, y);
}

static void do_aversive_gaze(void)
{
    /* Desvia para a lateral oposta à posição atual, amplitude maior */
    float cur_x, cur_y;
    gaze_service_get_current(&cur_x, &cur_y);

    float x = (cur_x >= 0.0f) ? -(0.45f + rand01() * 0.15f)
                               :  (0.45f + rand01() * 0.15f);
    float y = rand11() * 0.20f;
    gaze_service_set_target(x, y);
    ESP_LOGD(TAG, "aversive gaze → (%.2f, %.2f)", x, y);
}

static void reset_timers_and_center(void)
{
    s_saccade_timer_ms  = rand_interval(SACCADE_MIN_MS,  SACCADE_RANGE_MS);
    s_aversive_timer_ms = rand_interval(AVERSIVE_MIN_MS, AVERSIVE_RANGE_MS);
    s_yawn_timer_ms     = rand_interval(YAWN_MIN_MS,     YAWN_RANGE_MS);
    gaze_service_set_target(0.0f, 0.0f);
}

/* ── API ─────────────────────────────────────────────────────────────────── */

esp_err_t idle_service_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;

    s_saccade_timer_ms  = rand_interval(SACCADE_MIN_MS,  SACCADE_RANGE_MS);
    s_aversive_timer_ms = rand_interval(AVERSIVE_MIN_MS, AVERSIVE_RANGE_MS);
    s_yawn_timer_ms     = rand_interval(YAWN_MIN_MS,     YAWN_RANGE_MS);
    s_yawn_hold_ms      = 0;
    s_was_active        = false;
    s_initialized       = true;

    ESP_LOGI(TAG, "idle_service inicializado");
    return ESP_OK;
}

void idle_service_update(uint32_t dt_ms)
{
    if (!s_initialized) return;

    nb_robot_state_t state = state_machine_get_state();
    bool is_idle      = (state == NB_STATE_IDLE);
    bool is_attentive = (state == NB_STATE_ATTENTIVE);
    bool is_active    = is_idle || is_attentive;

    /* Transição de IDLE/ATTENTIVE → outro estado: centraliza gaze e reseta timers */
    if (!is_active) {
        if (s_was_active) {
            if (s_yawn_hold_ms > 0) {
                /* Cancela yawn em curso */
                expression_service_set(NB_EXPR_NEUTRAL, YAWN_TRANS_OUT_MS);
                s_yawn_hold_ms = 0;
            }
            reset_timers_and_center();
        }
        s_was_active = false;
        return;
    }

    s_was_active = true;

    /* ── Holddown de yawn ── */
    if (s_yawn_hold_ms > 0) {
        if (dt_ms >= s_yawn_hold_ms) {
            s_yawn_hold_ms = 0;
            expression_service_set(NB_EXPR_NEUTRAL, YAWN_TRANS_OUT_MS);
            ESP_LOGD(TAG, "yawn terminou");
        } else {
            s_yawn_hold_ms -= dt_ms;
        }
        /* Timers pausados durante yawn (não queremos saccade em meio ao bocejo) */
        return;
    }

    /* ── Micro-saccade (IDLE e ATTENTIVE) ── */
    if (s_saccade_timer_ms <= dt_ms) {
        do_micro_saccade();
        s_saccade_timer_ms = rand_interval(SACCADE_MIN_MS, SACCADE_RANGE_MS);
    } else {
        s_saccade_timer_ms -= dt_ms;
    }

    /* ── Aversive gaze (ATTENTIVE somente) ── */
    if (is_attentive) {
        if (s_aversive_timer_ms <= dt_ms) {
            do_aversive_gaze();
            s_aversive_timer_ms = rand_interval(AVERSIVE_MIN_MS, AVERSIVE_RANGE_MS);
        } else {
            s_aversive_timer_ms -= dt_ms;
        }
    } else {
        /* Não em ATTENTIVE: mantém timer pronto para quando entrar */
        s_aversive_timer_ms = rand_interval(AVERSIVE_MIN_MS, AVERSIVE_RANGE_MS);
    }

    /* ── Yawn (IDLE somente) ── */
    if (is_idle) {
        if (s_yawn_timer_ms <= dt_ms) {
            expression_service_set(NB_EXPR_SLEEPY, YAWN_TRANS_IN_MS);
            s_yawn_hold_ms  = YAWN_DURATION_MS;
            s_yawn_timer_ms = rand_interval(YAWN_MIN_MS, YAWN_RANGE_MS);
            ESP_LOGI(TAG, "yawn!");
        } else {
            s_yawn_timer_ms -= dt_ms;
        }
    } else {
        s_yawn_timer_ms = rand_interval(YAWN_MIN_MS, YAWN_RANGE_MS);
    }

    /* ── Stub: micro-neck-movement ── */
    /* TODO(etapa-3.3): quando motion_service estiver liberado, adicionar
     * chamadas a motion_neck_tilt() aqui (amplitude <5°, ≤3/min). */
}
