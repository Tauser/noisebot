/*
 * idle_service.c — Microbehaviors de idle do NoiseBot (Layer 5)
 *
 * Todos os timers são aleatórios (intervalos min+rand*range) para evitar
 * periodicidade mecânica. O hardware RNG do ESP32 garante entropia real.
 *
 * Comportamento por estado:
 *   IDLE:                motif raro (15–40s), distribuição rica em motifs
 *                        sustentados (CURIOUS_TILT, HEAD_TILT_HOLD).
 *                        Vida vem de blink + drift + motifs longos.
 *   ATTENTIVE:           motif frequente (5–13s) com distribuição variada,
 *                        + aversive gaze a cada 8–15s.
 *   IDLE somente:        yawn a cada 60–180s.
 *   Outros estados:      timers resetados, gaze retorna a center (0, 0),
 *                        overlay assimétrico limpo.
 *
 * Tipos de motif:
 *   bi-lateral       : peek esq → centro → peek dir (ou inverso).
 *   vert sweep       : peek baixo → centro → peek cima (ou inverso).
 *   wander           : lateral → vertical aleatório → lateral oposto → centro.
 *   curious check    : flash CURIOUS + lateral micro + micro cima.
 *   head tilt hold   : postura assimétrica de abertura 5–15s (overlay dopen).
 *   curious tilt     : CURIOUS sustentado 3.5–5s (motif principal de IDLE).
 *
 * Calibrado contra vídeo idle do EMO — ver docs/IDLE_REFERENCE.md.
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

/* Intervalo entre início de motifs:
 *   IDLE:      1.5–5s entre motifs — robô em repouso mas vivo.
 *              O vídeo EMO mostrou ~2 motifs LONGOS em 30s; aqui priorizamos
 *              presença visual sobre fidelidade estrita ao ref (companion desktop).
 *   ATTENTIVE: 1.0–3.5s — mais reativo.
 * Dentro de cada motif os steps têm gaps próprios (inner/outer).
 * Ver docs/IDLE_REFERENCE.md §1 e §4.1. */
#define SACCADE_IDLE_MIN_MS        1500U    /* IDLE: novo motif a cada 1.5–5s     */
#define SACCADE_IDLE_RANGE_MS      3500U
#define SACCADE_ATTENTIVE_MIN_MS   1000U    /* ATTENTIVE: 1.0–3.5 s               */
#define SACCADE_ATTENTIVE_RANGE_MS 2500U

#define AVERSIVE_MIN_MS       8000U
#define AVERSIVE_RANGE_MS     7000U

#define YAWN_ENABLED             0
#define YAWN_MIN_MS          60000U
#define YAWN_RANGE_MS       120000U
#define YAWN_DURATION_MS     2500.0f
#define YAWN_TRANS_MS         800.0f

#define ALONE_THRESHOLD_MS  300000U

/* ── Parâmetros de glance ─────────────────────────────────────────────────── */

/*
 * Amplitudes de gaze:
 *   peek  = olhada decisiva, claramente visível.
 *   micro = passo interno de um motif ou confirmação após peek.
 *
 * Referência: GAZE_X_TRAVEL_PX≈45, GAZE_Y_TRAVEL_PX≈24.
 *   peek lateral 0.18–0.38 → 8–17 px  (visível sem ser extremo)
 *   peek vertical 0.12–0.22 → 3–5 px  (olho menor: range mais curto)
 */
#define GLANCE_LAT_MICRO_MIN       0.06f
#define GLANCE_LAT_MICRO_RNG       0.07f    /* 0.06–0.13 */
#define GLANCE_LAT_PEEK_MIN        0.18f
#define GLANCE_LAT_PEEK_RNG        0.20f    /* 0.18–0.38 */

#define GLANCE_VERT_MICRO_MIN      0.05f
#define GLANCE_VERT_MICRO_RNG      0.06f    /* 0.05–0.11 */
#define GLANCE_VERT_PEEK_MIN       0.12f
#define GLANCE_VERT_PEEK_RNG       0.10f    /* 0.12–0.22 */

#define GLANCE_HOLD_MICRO_MIN_MS   150U
#define GLANCE_HOLD_MICRO_RNG_MS   150U     /* 150–300ms */
#define GLANCE_HOLD_PEEK_MIN_MS    250U
#define GLANCE_HOLD_PEEK_RNG_MS    300U     /* 250–550ms */

#define MOTIF_INNER_GAP_MIN_MS     280U    /* gap entre steps dentro do motif     */
#define MOTIF_INNER_GAP_RNG_MS     420U    /* 280–700ms — snappy, sem arrastar    */
#define MOTIF_OUTER_GAP_MIN_MS     600U    /* pausa após fim do motif             */
#define MOTIF_OUTER_GAP_RNG_MS    1200U    /* 600–1800ms — respira antes do próx  */

#define CURIOUS_FLASH_MIN_MS       180U
#define CURIOUS_FLASH_RANGE_MS     240U
#define CURIOUS_FLASH_TRANS_MS     80.0f

/* ── HEAD_TILT_HOLD ────────────────────────────────────────────────────────
 * Postura assimétrica vertical sustentada — olho L mais baixo que R (ou
 * vice-versa). No vídeo do EMO: ~+8 px de offset por 12s contínuos.
 * Sinal random; magnitude 0.08–0.12; duração 5–15s.                       */
#define TILT_HOLD_DY_MIN           0.08f
#define TILT_HOLD_DY_RNG           0.04f
#define TILT_HOLD_DUR_MIN_MS       5000U
#define TILT_HOLD_DUR_RNG_MS      10000U

/* ── CURIOUS_TILT ──────────────────────────────────────────────────────────
 * Versão sustentada de CURIOUS_CHECK. Toca a expressão CURIOUS (já
 * assimétrica no nosso modelo) por 3.5–5.0s, com transição mais lenta.
 * No vídeo: t=21.5s–26.0s e t=27.5s–30.0s.                                 */
#define CURIOUS_TILT_DUR_MIN_MS    3500U
#define CURIOUS_TILT_DUR_RNG_MS    1500U
#define CURIOUS_TILT_TRANS_MS      280.0f

/* ── Parâmetros de expressões involuntárias ──────────────────────────────── */

#define INVOLUNTARY_MIN_MS     4500U   /* janela mínima entre expressões        */
#define INVOLUNTARY_RANGE_MS   9000U   /* variação adicional                    */
#define INVOLUNTARY_PROB       0.07f   /* probabilidade de disparo por janela   */

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static inline float rand01(void)
{
    return (float)(esp_random() >> 1) / 2147483648.0f;
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

typedef enum {
    IDLE_MOTIF_NONE = 0,
    IDLE_MOTIF_GAZE_H,            /* horizontal: esq → centro → dir (ou inverso)    */
    IDLE_MOTIF_GAZE_V,            /* vertical: baixo → centro → cima (ou inverso)   */
    IDLE_MOTIF_GAZE_CORNERS,      /* ciclo pelos 4 cantos → centro (horário ou inv.) */
    IDLE_MOTIF_EXPR_CURIOUS_FLASH,/* flash curto de CURIOUS + micro gaze            */
    IDLE_MOTIF_POSE_TILT,         /* overlay assimétrico dopen 5–15s                */
    IDLE_MOTIF_EXPR_CURIOUS_HOLD, /* CURIOUS sustentado 3.5–5s                      */
} idle_motif_t;

static idle_motif_t s_motif              = IDLE_MOTIF_NONE;
static uint8_t      s_motif_step         = 0;
static float        s_motif_sign         = 1.0f;
static uint32_t     s_next_glance_ms     = 0;

/* Intervalo entre motifs depende do estado: IDLE usa janela longa (motif raro),
 * ATTENTIVE usa janela curta (motif frequente). is_idle_now=true para IDLE puro;
 * caso contrário (ATTENTIVE ou call-site sem contexto) usa janela ATTENTIVE.
 * Ver docs/IDLE_REFERENCE.md §4.1. */
static inline uint32_t saccade_interval_for(bool is_idle_now)
{
    taskENTER_CRITICAL(&s_mult_mux);
    float mult = s_saccade_mult;
    taskEXIT_CRITICAL(&s_mult_mux);
    uint32_t base = is_idle_now
        ? rand_interval(SACCADE_IDLE_MIN_MS,      SACCADE_IDLE_RANGE_MS)
        : rand_interval(SACCADE_ATTENTIVE_MIN_MS, SACCADE_ATTENTIVE_RANGE_MS);
    return (uint32_t)((float)base / mult);
}

/* Backwards-compat: usado em init/reset, antes de termos contexto de estado. */
static inline uint32_t saccade_interval(void)
{
    return saccade_interval_for(false /* assume ATTENTIVE para inicialização */);
}

/* ── Behaviors ───────────────────────────────────────────────────────────── */

static inline float lateral_micro(float sign)
{
    return sign * (GLANCE_LAT_MICRO_MIN + rand01() * GLANCE_LAT_MICRO_RNG);
}

static inline float lateral_peek(float sign)
{
    return sign * (GLANCE_LAT_PEEK_MIN + rand01() * GLANCE_LAT_PEEK_RNG);
}

static inline float vertical_micro(float sign)
{
    return sign * (GLANCE_VERT_MICRO_MIN + rand01() * GLANCE_VERT_MICRO_RNG);
}

static inline float vertical_peek(float sign)
{
    return sign * (GLANCE_VERT_PEEK_MIN + rand01() * GLANCE_VERT_PEEK_RNG);
}

static void schedule_inner_step(void)
{
    s_next_glance_ms = rand_interval(MOTIF_INNER_GAP_MIN_MS, MOTIF_INNER_GAP_RNG_MS);
}

static void schedule_outer_step(void)
{
    s_next_glance_ms = rand_interval(MOTIF_OUTER_GAP_MIN_MS, MOTIF_OUTER_GAP_RNG_MS);
}

/* Distribuição de motifs.
 *
 * Todos os motifs só iniciam quando a expressão ativa é NEUTRAL —
 * movimentos de gaze e expressão pressupõem NEUTRAL como base.
 * Se outra expressão estiver ativa (HAPPY, CURIOUS via behavior, etc.),
 * nenhum motif é iniciado neste ciclo.
 *
 * IDLE — mix equilibrado de gaze, expressão e postura:
 *   25% GAZE_H              horizontal esq ↔ dir
 *   20% GAZE_V              vertical cima ↕ baixo
 *   15% GAZE_CORNERS        ciclo pelos 4 cantos
 *   25% EXPR_CURIOUS_HOLD   CURIOUS sustentado (expressão principal)
 *   15% POSE_TILT           inclinação assimétrica (postura)
 *
 * ATTENTIVE — mais reativo, mais expressão curiosa:
 *   25% GAZE_H
 *   25% GAZE_V
 *   15% GAZE_CORNERS
 *   20% EXPR_CURIOUS_FLASH  flash rápido de CURIOUS + micro gaze
 *   15% EXPR_CURIOUS_HOLD
 */
static void begin_idle_motif(bool is_idle_now)
{
    if (emotion_model_get_expression() != NB_EXPR_NEUTRAL) {
        s_motif = IDLE_MOTIF_NONE;
        return;
    }

    float r = rand01();
    if (is_idle_now) {
        if      (r < 0.25f) s_motif = IDLE_MOTIF_GAZE_H;
        else if (r < 0.45f) s_motif = IDLE_MOTIF_GAZE_V;
        else if (r < 0.60f) s_motif = IDLE_MOTIF_GAZE_CORNERS;
        else if (r < 0.85f) s_motif = IDLE_MOTIF_EXPR_CURIOUS_HOLD;
        else                s_motif = IDLE_MOTIF_POSE_TILT;
    } else {
        /* ATTENTIVE */
        if      (r < 0.25f) s_motif = IDLE_MOTIF_GAZE_H;
        else if (r < 0.50f) s_motif = IDLE_MOTIF_GAZE_V;
        else if (r < 0.65f) s_motif = IDLE_MOTIF_GAZE_CORNERS;
        else if (r < 0.85f) s_motif = IDLE_MOTIF_EXPR_CURIOUS_FLASH;
        else                s_motif = IDLE_MOTIF_EXPR_CURIOUS_HOLD;
    }
    s_motif_step = 0;
    s_motif_sign = rand_sign();
}

static void finish_idle_motif(void)
{
    s_motif = IDLE_MOTIF_NONE;
    s_motif_step = 0;
    schedule_outer_step();
}

static void do_center_pause(void)
{
    gaze_service_set_target(0.0f, 0.0f);
    schedule_inner_step();
}

static void do_axis_glance(float x, float y, uint32_t hold_ms)
{
    gaze_service_glance(x, y, hold_ms);
    schedule_inner_step();
    ESP_LOGD(TAG, "idle motif glance → (%.2f, %.2f) hold=%lums",
             x, y, (unsigned long)hold_ms);
}

static void do_glance(bool is_idle_now)
{
    if (s_motif == IDLE_MOTIF_NONE) {
        begin_idle_motif(is_idle_now);
    }

    switch (s_motif) {
        /*
         * GAZE_H — varredura horizontal: peek lateral → centro → peek oposto.
         */
        case IDLE_MOTIF_GAZE_H:
            switch (s_motif_step++) {
                case 0:
                    do_axis_glance(lateral_peek(s_motif_sign), 0.0f,
                                   rand_interval(GLANCE_HOLD_PEEK_MIN_MS,
                                                 GLANCE_HOLD_PEEK_RNG_MS));
                    break;
                case 1:
                    do_center_pause();
                    break;
                case 2:
                    do_axis_glance(lateral_peek(-s_motif_sign), 0.0f,
                                   rand_interval(GLANCE_HOLD_PEEK_MIN_MS,
                                                 GLANCE_HOLD_PEEK_RNG_MS));
                    break;
                case 3:
                    do_center_pause();
                    break;
                default:
                    finish_idle_motif();
                    break;
            }
            break;

        /*
         * GAZE_V — varredura vertical: peek cima/baixo → centro → peek oposto.
         * s_motif_sign determina se começa para cima (-1) ou para baixo (+1).
         */
        case IDLE_MOTIF_GAZE_V:
            switch (s_motif_step++) {
                case 0:
                    do_axis_glance(0.0f, vertical_peek(s_motif_sign),
                                   rand_interval(GLANCE_HOLD_PEEK_MIN_MS,
                                                 GLANCE_HOLD_PEEK_RNG_MS));
                    break;
                case 1:
                    do_center_pause();
                    break;
                case 2:
                    do_axis_glance(0.0f, vertical_peek(-s_motif_sign),
                                   rand_interval(GLANCE_HOLD_PEEK_MIN_MS,
                                                 GLANCE_HOLD_PEEK_RNG_MS));
                    break;
                case 3:
                    do_center_pause();
                    break;
                default:
                    finish_idle_motif();
                    break;
            }
            break;

        /*
         * GAZE_CORNERS — ciclo pelos 4 cantos em sentido horário ou anti-horário.
         * sign=+1: canto sup-esq → sup-dir → inf-dir → inf-esq → centro.
         * sign=-1: canto sup-dir → sup-esq → inf-esq → inf-dir → centro.
         */
        case IDLE_MOTIF_GAZE_CORNERS:
            switch (s_motif_step++) {
                case 0:
                    do_axis_glance(lateral_peek(-s_motif_sign), vertical_peek(-1.0f),
                                   rand_interval(GLANCE_HOLD_PEEK_MIN_MS,
                                                 GLANCE_HOLD_PEEK_RNG_MS));
                    break;
                case 1:
                    do_axis_glance(lateral_peek(s_motif_sign), vertical_peek(-1.0f),
                                   rand_interval(GLANCE_HOLD_PEEK_MIN_MS,
                                                 GLANCE_HOLD_PEEK_RNG_MS));
                    break;
                case 2:
                    do_axis_glance(lateral_peek(s_motif_sign), vertical_peek(1.0f),
                                   rand_interval(GLANCE_HOLD_PEEK_MIN_MS,
                                                 GLANCE_HOLD_PEEK_RNG_MS));
                    break;
                case 3:
                    do_axis_glance(lateral_peek(-s_motif_sign), vertical_peek(1.0f),
                                   rand_interval(GLANCE_HOLD_PEEK_MIN_MS,
                                                 GLANCE_HOLD_PEEK_RNG_MS));
                    break;
                case 4:
                    do_center_pause();
                    break;
                default:
                    finish_idle_motif();
                    break;
            }
            break;

        /*
         * EXPR_CURIOUS_FLASH — flash curto de CURIOUS + micro gaze lateral e cima.
         */
        case IDLE_MOTIF_EXPR_CURIOUS_FLASH:
            switch (s_motif_step++) {
                case 0: {
                    uint32_t dur = rand_interval(CURIOUS_FLASH_MIN_MS,
                                                 CURIOUS_FLASH_RANGE_MS);
                    expression_play(NB_EXPR_CURIOUS, (float)dur,
                                    CURIOUS_FLASH_TRANS_MS);
                    do_axis_glance(lateral_micro(s_motif_sign), 0.0f,
                                   rand_interval(GLANCE_HOLD_MICRO_MIN_MS,
                                                 GLANCE_HOLD_MICRO_RNG_MS));
                    break;
                }
                case 1:
                    do_center_pause();
                    break;
                case 2:
                    do_axis_glance(0.0f, vertical_micro(-1.0f),
                                   rand_interval(GLANCE_HOLD_MICRO_MIN_MS,
                                                 GLANCE_HOLD_MICRO_RNG_MS));
                    break;
                default:
                    finish_idle_motif();
                    break;
            }
            break;

        /*
         * POSE_TILT — rotação real dos olhos via sprite pushRotateZoom.
         * Ambos os olhos giram no mesmo sentido (simulando roll da cabeça).
         * sign=+1: horário (~30°); sign=-1: anti-horário (~-30°).
         * Mantém 5–15s; blink e drift seguem ativos.
         */
        case IDLE_MOTIF_POSE_TILT:
            switch (s_motif_step++) {
                case 0: {
                    float angle = 4.0f * s_motif_sign;
                    expression_service_set_idle_rotation(angle, angle);
                    s_next_glance_ms = rand_interval(TILT_HOLD_DUR_MIN_MS,
                                                     TILT_HOLD_DUR_RNG_MS);
                    ESP_LOGD(TAG, "pose_tilt angle=%.1f dur=%lums",
                             angle, (unsigned long)s_next_glance_ms);
                    break;
                }
                case 1:
                    expression_service_set_idle_rotation(0.0f, 0.0f);
                    schedule_inner_step();
                    break;
                default:
                    finish_idle_motif();
                    break;
            }
            break;

        /*
         * EXPR_CURIOUS_HOLD — CURIOUS sustentado 3.5–5s com transição suave.
         * Motif principal de expressão no IDLE.
         */
        case IDLE_MOTIF_EXPR_CURIOUS_HOLD:
            switch (s_motif_step++) {
                case 0: {
                    uint32_t dur = rand_interval(CURIOUS_TILT_DUR_MIN_MS,
                                                 CURIOUS_TILT_DUR_RNG_MS);
                    expression_play(NB_EXPR_CURIOUS, (float)dur,
                                    CURIOUS_TILT_TRANS_MS);
                    s_next_glance_ms = dur + (uint32_t)CURIOUS_TILT_TRANS_MS;
                    ESP_LOGD(TAG, "expr_curious_hold dur=%lums", (unsigned long)dur);
                    break;
                }
                default:
                    finish_idle_motif();
                    break;
            }
            break;

        case IDLE_MOTIF_NONE:
        default:
            finish_idle_motif();
            break;
    }
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
    ESP_LOGD(TAG, "aversive gaze -> (%.2f, %.2f)", x, y);
}

static void reset_timers_and_center(void)
{
    s_saccade_timer_ms      = saccade_interval();
    s_aversive_timer_ms     = rand_interval(AVERSIVE_MIN_MS, AVERSIVE_RANGE_MS);
    s_yawn_timer_ms         = rand_interval(YAWN_MIN_MS,     YAWN_RANGE_MS);
    s_motif                 = IDLE_MOTIF_NONE;
    s_motif_step            = 0;
    s_next_glance_ms        = 0;
    gaze_service_set_anchor(0.0f, 0.0f);
    gaze_service_set_target(0.0f, 0.0f);
    /* Limpa overlay assimetrico e rotacao — garante baseline IDLE limpo. */
    expression_service_set_idle_overlay(0.0f, 0.0f, 0.0f, 0.0f);
    expression_service_set_idle_rotation(0.0f, 0.0f);
}

/* -- API -- */

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

        /* Transicao de IDLE/ATTENTIVE -> outro estado: centraliza gaze e reseta timers */
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

    /* Transicao de entrada/saida de IDLE: reseta timer de solidao e
     * recalcula a janela de saccade para a nova distribuicao.
     * Em entrada IDLE, limpa overlay para honrar a regra de baseline. */
    if (is_idle != s_was_idle) {
        s_alone_timer_ms = 0;
        if (is_idle) {
            gaze_service_set_anchor(0.0f, 0.0f);
            gaze_service_set_target(0.0f, 0.0f);
            s_motif          = IDLE_MOTIF_NONE;
            s_motif_step     = 0;
            s_next_glance_ms = 0;
            expression_service_set_idle_overlay(0.0f, 0.0f, 0.0f, 0.0f);
            expression_service_set_idle_rotation(0.0f, 0.0f);
        }
        s_saccade_timer_ms = saccade_interval_for(is_idle);
    }

    s_was_active = true;
    s_was_idle   = is_idle;
    expression_service_set_breath_enabled(true);

    /* -- Glance (IDLE e ATTENTIVE) -- */
    if (s_saccade_timer_ms <= dt_ms) {
        do_glance(is_idle);
        if (s_next_glance_ms > 0u) {
            s_saccade_timer_ms = s_next_glance_ms;
            s_next_glance_ms   = 0u;
        } else {
            s_saccade_timer_ms = saccade_interval_for(is_idle);
        }
    } else {
        s_saccade_timer_ms -= dt_ms;
    }

    /* -- Aversive gaze (ATTENTIVE somente) -- */
    if (is_attentive) {
        if (s_aversive_timer_ms <= dt_ms) {
            do_aversive_gaze();
            s_aversive_timer_ms = rand_interval(AVERSIVE_MIN_MS, AVERSIVE_RANGE_MS);
        } else {
            s_aversive_timer_ms -= dt_ms;
        }
    } else {
        s_aversive_timer_ms = rand_interval(AVERSIVE_MIN_MS, AVERSIVE_RANGE_MS);
    }

    /* -- Yawn (IDLE somente) -- */
    if (is_idle) {
        if (!YAWN_ENABLED) {
            s_yawn_timer_ms = rand_interval(YAWN_MIN_MS, YAWN_RANGE_MS);
        } else if (rhythm_service_is_locked()) {
            s_yawn_timer_ms = rand_interval(YAWN_MIN_MS, YAWN_RANGE_MS);
        } else if (s_yawn_timer_ms <= dt_ms) {
            expression_play(NB_EXPR_SLEEPY, YAWN_DURATION_MS, YAWN_TRANS_MS);
            float attn = attention_service_get_level();
            taskENTER_CRITICAL(&s_mult_mux);
            float yawn_mult = s_yawn_mult;
            taskEXIT_CRITICAL(&s_mult_mux);
            float scale = (1.0f + attn * 2.0f) * yawn_mult;
            s_yawn_timer_ms = (uint32_t)((float)rand_interval(YAWN_MIN_MS, YAWN_RANGE_MS) * scale);
            ESP_LOGI(TAG, "yawn! (proximo em %lums, attn=%.2f)", (unsigned long)s_yawn_timer_ms, attn);
        } else {
            s_yawn_timer_ms -= dt_ms;
        }
    } else {
        s_yawn_timer_ms = rand_interval(YAWN_MIN_MS, YAWN_RANGE_MS);
    }

    /* -- Alone timer (IDLE somente) -- */
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

    /* -- Expressoes involuntarias (IDLE somente) -- */
    if (is_idle) {
        if (s_involuntary_ms <= dt_ms) {
            if (rand01() < INVOLUNTARY_PROB) {
                nb_expression_t cur = emotion_model_get_expression();
                if (cur == NB_EXPR_NEUTRAL) {
                    expression_play(NB_EXPR_FOCUSED, 120.0f + rand01() * 120.0f, 45.0f);
                    ESP_LOGD(TAG, "involuntary: neutral focus pulse");
                } else if (cur == NB_EXPR_HAPPY) {
                    expression_play(NB_EXPR_FOCUSED, 100.0f, 45.0f);
                    ESP_LOGD(TAG, "involuntary: happy focus pulse");
                } else if (cur == NB_EXPR_FOCUSED) {
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

    /* -- Stub: micro-neck-movement -- */
    /* TODO(etapa-3.3): quando motion_service estiver liberado, adicionar
     * chamadas a motion_neck_tilt() aqui (amplitude <5deg, <=3/min). */
}
