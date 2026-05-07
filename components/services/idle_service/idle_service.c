/*
 * idle_service.c — Microbehaviors de idle do NoiseBot (Layer 5)
 *
 * Todos os timers são aleatórios (intervalos min+rand*range) para evitar
 * periodicidade mecânica. O hardware RNG do ESP32 garante entropia real.
 *
 * Comportamento por estado:
 *   IDLE || ATTENTIVE:  glance a cada 1.5–4.5s (com retorno automático ao centro).
 *   ATTENTIVE somente:  aversive gaze a cada 8–15s.
 *   IDLE somente:       yawn a cada 60–180s.
 *   Outros estados:     timers resetados, gaze retorna a center (0, 0).
 *
 * Tipos de glance em IDLE:
 *   curious flash: microexpressão curiosa curta com gaze lateral puro.
 *   soft center   (20%): volta ao âncora sem glance ativo.
 *   double glance (15%): glance normal + segundo glance agendado 100–250ms depois.
 *   single glance (45%): glance simples com retorno automático.
 *
 * Nota: micro-neck-movement (<5°, ≤3/min) requer motion_service (Etapa 3.3).
 * Stub preparado — ativado quando motion for liberado.
 */

#include "idle_service.h"
#include "gaze_service.h"
#include "expression_service.h"
#include "state_machine.h"
#include "attention_service.h"
#include "rhythm_service.h"
#include "emotion_model.h"

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

#define TAG "nb_idle"

/* ── Parâmetros de timing ────────────────────────────────────────────────── */

#define SACCADE_MIN_MS         700U    /* intervalo mínimo entre glances        */
#define SACCADE_RANGE_MS      1500U    /* variação adicional (sorteada)         */

#define AVERSIVE_MIN_MS       8000U
#define AVERSIVE_RANGE_MS     7000U

#define YAWN_ENABLED             1
#define YAWN_MIN_MS          60000U
#define YAWN_RANGE_MS       120000U
#define YAWN_DURATION_MS     2500.0f
#define YAWN_TRANS_MS         800.0f

#define ALONE_THRESHOLD_MS  300000U

/* ── Parâmetros de glance ─────────────────────────────────────────────────── */

/** Distribuição dos glances: laterais pequenos são o padrão; cantos são raros. */
#define GLANCE_LATERAL_PROB   0.88f
#define GLANCE_VERTICAL_PROB  0.12f

#define GLANCE_LATERAL_X_MIN  0.22f   /* start mais decisivo (era 0.14) */
#define GLANCE_LATERAL_X_RNG  0.16f   /* range 0.22–0.38, longe dos cantos */
#define GLANCE_VERTICAL_Y_MIN 0.09f   /* visível: 0.09×32px = 2.9px (era 0.035) */
#define GLANCE_VERTICAL_Y_RNG 0.06f   /* range 0.09–0.15 */

/** Tempo de hold antes de retornar ao âncora (ms). */
#define GLANCE_HOLD_MIN_MS      90U
#define GLANCE_HOLD_RANGE_MS   260U    /* total: 90–350ms */

/** Probabilidade de curious flash: microexpressão curiosa curta em IDLE. */
#define CURIOUS_FLASH_PROB    0.16f

/** Duração do curious flash. */
#define CURIOUS_FLASH_MIN_MS   300U
#define CURIOUS_FLASH_RANGE_MS 400U
#define CURIOUS_FLASH_TRANS_MS 80.0f

/** Probabilidade de soft center reset (volta ao âncora sem glance ativo). */
#define GLANCE_CENTER_PROB    0.08f

/** Probabilidade de double glance (segundo glance após o primeiro). */
#define GLANCE_DOUBLE_PROB    0.24f

/** Delay entre os dois glances do double glance (ms). */
#define GLANCE_DOUBLE_DELAY_MIN_MS   100U
#define GLANCE_DOUBLE_DELAY_RANGE_MS 150U

/* ── Parâmetros de expressões involuntárias ──────────────────────────────── */

#define INVOLUNTARY_MIN_MS     6000U   /* janela mínima entre expressões        */
#define INVOLUNTARY_RANGE_MS   8000U   /* variação adicional                    */
#define INVOLUNTARY_PROB       0.05f   /* probabilidade de disparo por janela         */

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static inline float rand01(void)
{
    return (float)(esp_random() >> 1) / 2147483648.0f;
}

static inline float rand11(void)
{
    return (float)(int32_t)esp_random() / 2147483648.0f;
}

static inline uint32_t rand_interval(uint32_t min_ms, uint32_t range_ms)
{
    return min_ms + (uint32_t)((float)range_ms * rand01());
}

static inline float rand_sign(void)
{
    return rand01() < 0.5f ? -1.0f : 1.0f;
}

/* ── Estado interno ──────────────────────────────────────────────────────── */

static bool     s_initialized       = false;

static uint32_t s_saccade_timer_ms  = 0;
static uint32_t s_aversive_timer_ms = 0;
static uint32_t s_yawn_timer_ms     = 0;
static uint32_t s_alone_timer_ms    = 0;

static bool     s_was_active        = false;
static bool     s_was_idle          = false;
static float    s_saccade_mult      = 1.0f;
static float    s_yawn_mult         = 1.0f;
static uint32_t s_involuntary_ms    = 0;
static portMUX_TYPE s_mult_mux      = portMUX_INITIALIZER_UNLOCKED;

static nb_idle_alone_cb_t s_alone_cb = NULL;

/* Double glance pendente */
static bool     s_double_glance_pending  = false;
static uint32_t s_double_glance_ms       = 0;
static float    s_double_glance_x        = 0.0f;
static float    s_double_glance_y        = 0.0f;
static uint32_t s_double_glance_hold_ms  = 0;

static inline uint32_t saccade_interval(void)
{
    taskENTER_CRITICAL(&s_mult_mux);
    float mult = s_saccade_mult;
    taskEXIT_CRITICAL(&s_mult_mux);
    uint32_t base = rand_interval(SACCADE_MIN_MS, SACCADE_RANGE_MS);
    return (uint32_t)((float)base / mult);
}

/* ── Behaviors ───────────────────────────────────────────────────────────── */

static void choose_glance_target(float *x, float *y)
{
    float kind = rand01();

    if (kind < GLANCE_LATERAL_PROB) {
        *x = rand_sign() * (GLANCE_LATERAL_X_MIN + rand01() * GLANCE_LATERAL_X_RNG);
        *y = 0.0f;
        return;
    }

    if (kind < GLANCE_LATERAL_PROB + GLANCE_VERTICAL_PROB) {
        *x = 0.0f;   /* cardinal puro — sem componente diagonal */
        *y = rand_sign() * (GLANCE_VERTICAL_Y_MIN + rand01() * GLANCE_VERTICAL_Y_RNG);
        return;
    }

    *x = rand_sign() * (GLANCE_LATERAL_X_MIN + rand01() * GLANCE_LATERAL_X_RNG);
    *y = 0.0f;
}

static void do_glance(void)
{
    float r = rand01();

    if (CURIOUS_FLASH_PROB > 0.0f && r < CURIOUS_FLASH_PROB) {
        /* Curious flash: expressão NB_EXPR_CURIOUS + glance lateral contido */
        float x      = rand_sign() * (GLANCE_LATERAL_X_MIN + rand01() * GLANCE_LATERAL_X_RNG * 0.6f);
        float y      = 0.0f;
        uint32_t hold = rand_interval(GLANCE_HOLD_MIN_MS, GLANCE_HOLD_RANGE_MS);
        uint32_t dur  = rand_interval(CURIOUS_FLASH_MIN_MS, CURIOUS_FLASH_RANGE_MS);
        expression_play(NB_EXPR_CURIOUS, (float)dur, CURIOUS_FLASH_TRANS_MS);
        gaze_service_glance(x, y, hold);
        ESP_LOGD(TAG, "curious flash → (%.2f, %.2f) hold=%lums", x, y, (unsigned long)hold);
        return;
    }

    if (r < CURIOUS_FLASH_PROB + GLANCE_CENTER_PROB) {
        /* Soft center: re-ancora no centro sem disparar glance */
        gaze_service_set_target(0.0f, 0.0f);
        ESP_LOGD(TAG, "soft center reset");
        return;
    }

    /* Glance normal com possível double */
    float x;
    float y;
    choose_glance_target(&x, &y);
    uint32_t hold = rand_interval(GLANCE_HOLD_MIN_MS, GLANCE_HOLD_RANGE_MS);
    gaze_service_glance(x, y, hold);

    if (!s_double_glance_pending && rand01() < GLANCE_DOUBLE_PROB) {
        /* Segundo glance mantém direção cardinal do primeiro — sem introduzir diagonal. */
        if (y != 0.0f) {
            /* Vertical: x permanece 0, y ligeiramente diferente */
            s_double_glance_x = 0.0f;
            s_double_glance_y = y * (0.55f + rand01() * 0.35f);
        } else {
            /* Lateral: x muda levemente, y permanece 0 */
            s_double_glance_x = x * 0.6f + rand_sign() * rand01() * GLANCE_LATERAL_X_RNG * 0.25f;
            s_double_glance_y = 0.0f;
        }
        s_double_glance_hold_ms = rand_interval(GLANCE_HOLD_MIN_MS, GLANCE_HOLD_RANGE_MS / 2U);
        s_double_glance_ms      = rand_interval(GLANCE_DOUBLE_DELAY_MIN_MS, GLANCE_DOUBLE_DELAY_RANGE_MS);
        s_double_glance_pending = true;
        ESP_LOGD(TAG, "double glance agendado → (%.2f, %.2f)", s_double_glance_x, s_double_glance_y);
    }

    ESP_LOGD(TAG, "glance → (%.2f, %.2f) hold=%lums", x, y, (unsigned long)hold);
}

static void do_aversive_gaze(void)
{
    /* Desvia para a lateral oposta à posição atual, amplitude maior */
    float cur_x, cur_y;
    gaze_service_get_current(&cur_x, &cur_y);

    float x = (cur_x >= 0.0f) ? -(0.45f + rand01() * 0.15f)
                               :  (0.45f + rand01() * 0.15f);
    float y = 0.0f;
    gaze_service_set_target(x, y);
    ESP_LOGD(TAG, "aversive gaze → (%.2f, %.2f)", x, y);
}

static void reset_timers_and_center(void)
{
    s_saccade_timer_ms      = saccade_interval();
    s_aversive_timer_ms     = rand_interval(AVERSIVE_MIN_MS, AVERSIVE_RANGE_MS);
    s_yawn_timer_ms         = rand_interval(YAWN_MIN_MS,     YAWN_RANGE_MS);
    s_double_glance_pending = false;
    gaze_service_set_anchor(0.0f, 0.0f);
    gaze_service_set_target(0.0f, 0.0f);
}

/* ── API ─────────────────────────────────────────────────────────────────── */

void idle_service_set_saccade_multiplier(float factor)
{
    if (factor < 0.1f) factor = 0.1f;
    if (factor > 5.0f) factor = 5.0f;
    taskENTER_CRITICAL(&s_mult_mux);
    s_saccade_mult = factor;
    taskEXIT_CRITICAL(&s_mult_mux);
}

void idle_service_set_yawn_multiplier(float factor)
{
    if (factor < 0.1f) factor = 0.1f;
    if (factor > 5.0f) factor = 5.0f;
    taskENTER_CRITICAL(&s_mult_mux);
    s_yawn_mult = factor;
    taskEXIT_CRITICAL(&s_mult_mux);
}

esp_err_t idle_service_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;

    s_saccade_timer_ms  = saccade_interval();
    s_aversive_timer_ms = rand_interval(AVERSIVE_MIN_MS, AVERSIVE_RANGE_MS);
    s_yawn_timer_ms     = rand_interval(YAWN_MIN_MS,     YAWN_RANGE_MS);
    s_alone_timer_ms    = 0;
    s_involuntary_ms    = rand_interval(INVOLUNTARY_MIN_MS, INVOLUNTARY_RANGE_MS);
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
            expression_service_set_breath_enabled(false);
        }
        s_was_active = false;
        s_was_idle   = false;
        return;
    }

    /* Transição de entrada/saída de IDLE: reseta timer de solidão */
    if (is_idle != s_was_idle) {
        s_alone_timer_ms = 0;
        if (is_idle) {
            gaze_service_set_anchor(0.0f, 0.0f);
            gaze_service_set_target(0.0f, 0.0f);
            s_double_glance_pending = false;
        }
    }

    s_was_active = true;
    s_was_idle   = is_idle;
    expression_service_set_breath_enabled(true);

    /* ── Double glance pendente ── */
    if (s_double_glance_pending) {
        if (s_double_glance_ms <= dt_ms) {
            gaze_service_glance(s_double_glance_x, s_double_glance_y, s_double_glance_hold_ms);
            s_double_glance_pending = false;
            ESP_LOGD(TAG, "double glance disparado → (%.2f, %.2f)", s_double_glance_x, s_double_glance_y);
        } else {
            s_double_glance_ms -= dt_ms;
        }
    }

    /* ── Glance (IDLE e ATTENTIVE) ── */
    if (s_saccade_timer_ms <= dt_ms) {
        do_glance();
        s_saccade_timer_ms = saccade_interval();
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
        /* Nunca boceja durante música com ritmo detectado */
        if (!YAWN_ENABLED) {
            s_yawn_timer_ms = rand_interval(YAWN_MIN_MS, YAWN_RANGE_MS);
        } else if (rhythm_service_is_locked()) {
            s_yawn_timer_ms = rand_interval(YAWN_MIN_MS, YAWN_RANGE_MS);
        } else if (s_yawn_timer_ms <= dt_ms) {
            expression_play(NB_EXPR_SLEEPY, YAWN_DURATION_MS, YAWN_TRANS_MS);
            /* Atenção alta suprime yawn: intervalo × (1 + attention × 2) */
            float attn = attention_service_get_level();
            taskENTER_CRITICAL(&s_mult_mux);
            float yawn_mult = s_yawn_mult;
            taskEXIT_CRITICAL(&s_mult_mux);
            float scale = (1.0f + attn * 2.0f) * yawn_mult;
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

    /* ── Expressões involuntárias (IDLE somente) ── */
    if (is_idle) {
        if (s_involuntary_ms <= dt_ms) {
            if (rand01() < INVOLUNTARY_PROB) {
                nb_expression_t cur = emotion_model_get_expression();
                if (cur == NB_EXPR_NEUTRAL) {
                    ESP_LOGD(TAG, "involuntary: neutral skipped");
                } else if (cur == NB_EXPR_HAPPY) {
                    /* Piscar satisfeito: olho semicerrando brevemente */
                    expression_play(NB_EXPR_SLEEPY, 80.0f, 40.0f);
                    ESP_LOGD(TAG, "involuntary: satisfied blink");
                } else if (cur == NB_EXPR_FOCUSED) {
                    /* Micro-squint de concentração */
                    expression_play(NB_EXPR_SUSPICIOUS, 100.0f, 40.0f);
                    ESP_LOGD(TAG, "involuntary: micro-squint");
                }
            }
            s_involuntary_ms = rand_interval(INVOLUNTARY_MIN_MS, INVOLUNTARY_RANGE_MS);
        } else {
            s_involuntary_ms -= dt_ms;
        }
    } else {
        s_involuntary_ms = rand_interval(INVOLUNTARY_MIN_MS, INVOLUNTARY_RANGE_MS);
    }

    /* ── Stub: micro-neck-movement ── */
    /* TODO(etapa-3.3): quando motion_service estiver liberado, adicionar
     * chamadas a motion_neck_tilt() aqui (amplitude <5°, ≤3/min). */
}
