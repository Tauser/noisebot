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
#include "attention_service.h"

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
#define YAWN_DURATION_MS    2500.0f  /* tempo em SLEEPY antes de retornar     */
#define YAWN_TRANS_MS        800.0f  /* transição de entrada e saída          */

#define ALONE_THRESHOLD_MS 300000U   /* 5 min sem interação → solidão         */

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
static uint32_t s_alone_timer_ms    = 0;

static bool     s_was_active        = false; /* estava em IDLE/ATTENTIVE */
static bool     s_was_idle          = false; /* estava em IDLE (para reset do alone timer) */

static nb_idle_alone_cb_t s_alone_cb = NULL;

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
    s_alone_timer_ms    = 0;
    s_was_active        = false;
    s_was_idle          = false;
    s_initialized       = true;

    ESP_LOGI(TAG, "idle_service inicializado");
    return ESP_OK;
}

void idle_service_set_alone_cb(nb_idle_alone_cb_t cb)
{
    s_alone_cb = cb;
}

void idle_service_on_interaction(void)
{
    s_alone_timer_ms = 0;
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
            reset_timers_and_center();
            s_alone_timer_ms = 0;
        }
        s_was_active = false;
        s_was_idle   = false;
        return;
    }

    /* Transição de entrada/saída de IDLE: reseta timer de solidão */
    if (is_idle != s_was_idle) {
        s_alone_timer_ms = 0;
    }

    s_was_active = true;
    s_was_idle   = is_idle;

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
            expression_play(NB_EXPR_SLEEPY, YAWN_DURATION_MS, YAWN_TRANS_MS);
            /* Atenção alta suprime yawn: intervalo × (1 + attention × 2) */
            float attn = attention_service_get_level();
            float scale = 1.0f + attn * 2.0f;
            s_yawn_timer_ms = (uint32_t)((float)rand_interval(YAWN_MIN_MS, YAWN_RANGE_MS) * scale);
            ESP_LOGI(TAG, "yawn! (próximo em %lums, attn=%.2f)", (unsigned long)s_yawn_timer_ms, attn);
        } else {
            s_yawn_timer_ms -= dt_ms;
        }
    } else {
        s_yawn_timer_ms = rand_interval(YAWN_MIN_MS, YAWN_RANGE_MS);
    }

    /* ── Alone timer (IDLE somente) ── */
    if (is_idle) {
        s_alone_timer_ms += dt_ms;
        if (s_alone_timer_ms >= ALONE_THRESHOLD_MS) {
            s_alone_timer_ms = 0;
            ESP_LOGI(TAG, "alone threshold reached");
            if (s_alone_cb) {
                s_alone_cb();
            }
        }
    } else {
        s_alone_timer_ms = 0;
    }

    /* ── Stub: micro-neck-movement ── */
    /* TODO(etapa-3.3): quando motion_service estiver liberado, adicionar
     * chamadas a motion_neck_tilt() aqui (amplitude <5°, ≤3/min). */
}
