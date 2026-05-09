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
 * Tipos de glance:
 *   side peek        : olha para um lado, volta ao centro, repete menor.
 *   vertical scan    : olha para cima, centro, depois baixo leve.
 *   cross scan       : esquerda/direita e cima em micro sequência cardinal.
 *   curious check    : microexpressão curiosa curta com gaze contido.
 *   line blink       : barra horizontal breve (estilo EMO).
 *   head tilt hold   : postura assimétrica vertical 5–15s (overlay).
 *   look down blink  : gaze↓ + 2× blink-bar com hold entre eles.
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

#define YAWN_ENABLED             1
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

#define LINE_BLINK_MIN_MS           85U
#define LINE_BLINK_RANGE_MS         55U
#define LINE_BLINK_TRANS_MS        35.0f

/* ── HEAD_TILT_HOLD ────────────────────────────────────────────────────────
 * Postura assimétrica vertical sustentada — olho L mais baixo que R (ou
 * vice-versa). No vídeo do EMO: ~+8 px de offset por 12s contínuos.
 * Sinal random; magnitude 0.08–0.12; duração 5–15s.                       */
#define TILT_HOLD_DY_MIN           0.08f
#define TILT_HOLD_DY_RNG           0.04f
#define TILT_HOLD_DUR_MIN_MS       5000U
#define TILT_HOLD_DUR_RNG_MS      10000U

/* ── LOOK_DOWN_BLINK ───────────────────────────────────────────────────────
 * Sequência composta: gaze.y → +0.4, blink-bar, hold, segundo blink, return.
 * Total ~1.8–2.2s. No vídeo: t=18.0s–20.0s.                                */
#define LDB_GAZE_Y                 0.40f
#define LDB_HOLD_BETWEEN_MIN_MS     550U
#define LDB_HOLD_BETWEEN_RNG_MS     350U
#define LDB_GAZE_HOLD_MS           1800U
#define LDB_BLINK_DUR_MS             80U
#define LDB_BLINK_TRANS_MS          30.0f

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
    IDLE_MOTIF_SIDE_PEEK,         /* peek lateral + centro + micro mesmo lado  */
    IDLE_MOTIF_VERTICAL_SCAN,     /* cima → centro → baixo leve               */
    IDLE_MOTIF_CROSS_SCAN,        /* lateral → cima → lateral oposto → centro */
    IDLE_MOTIF_CURIOUS_CHECK,     /* flash CURIOUS + lateral micro + cima      */
    IDLE_MOTIF_LINE_BLINK,        /* blink-bar isolado curto                   */
    IDLE_MOTIF_HEAD_TILT_HOLD,    /* overlay assimétrico vertical 5–15s        */
    IDLE_MOTIF_LOOK_DOWN_BLINK,   /* gaze↓ + blink-bar + hold + blink-bar      */
    IDLE_MOTIF_CURIOUS_TILT,      /* CURIOUS sustentado 3.5–5s                 */
    /* Novos — variedade direcional solicitada pelo usuário. */
    IDLE_MOTIF_BI_LATERAL,        /* peek esq → centro → peek dir (ou inverso) */
    IDLE_MOTIF_VERT_SWEEP,        /* peek baixo → centro → peek cima (ou inv.) */
    IDLE_MOTIF_WANDER,            /* lateral → cima → lateral oposto → centro  */
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
 * IDLE — prioridade em movimento direcional visível (~75% com gaze ativo):
 *   18% SIDE_PEEK         peek lateral + micro mesmo lado
 *   18% BI_LATERAL        peek esq → centro → peek dir (ambos lados)
 *   15% VERT_SWEEP        baixo → centro → cima ou vice-versa
 *   12% CROSS_SCAN        lateral → cima → lateral oposto → centro
 *   12% WANDER            lateral → cima → lateral oposto → centro (aleatório)
 *   15% CURIOUS_TILT      CURIOUS sustentado (expressão)
 *    5% HEAD_TILT_HOLD    overlay assimétrico (postura)
 *    3% LOOK_DOWN_BLINK   gaze↓ + blinks
 *    2% LINE_BLINK        blink isolado
 *
 * ATTENTIVE — mais rápido, mais lateral/vertical:
 *   25% SIDE_PEEK
 *   20% BI_LATERAL
 *   20% VERTICAL_SCAN
 *   15% CURIOUS_CHECK
 *   10% CROSS_SCAN
 *    5% WANDER
 *    5% LINE_BLINK
 */
static void begin_idle_motif(bool is_idle_now)
{
    float r = rand01();
    if (is_idle_now) {
        if      (r < 0.18f) s_motif = IDLE_MOTIF_SIDE_PEEK;
        else if (r < 0.36f) s_motif = IDLE_MOTIF_BI_LATERAL;
        else if (r < 0.51f) s_motif = IDLE_MOTIF_VERT_SWEEP;
        else if (r < 0.63f) s_motif = IDLE_MOTIF_CROSS_SCAN;
        else if (r < 0.75f) s_motif = IDLE_MOTIF_WANDER;
        else if (r < 0.90f) s_motif = IDLE_MOTIF_CURIOUS_TILT;
        else if (r < 0.95f) s_motif = IDLE_MOTIF_HEAD_TILT_HOLD;
        else if (r < 0.98f) s_motif = IDLE_MOTIF_LOOK_DOWN_BLINK;
        else                s_motif = IDLE_MOTIF_LINE_BLINK;
    } else {
        /* ATTENTIVE */
        if      (r < 0.25f) s_motif = IDLE_MOTIF_SIDE_PEEK;
        else if (r < 0.45f) s_motif = IDLE_MOTIF_BI_LATERAL;
        else if (r < 0.65f) s_motif = IDLE_MOTIF_VERTICAL_SCAN;
        else if (r < 0.80f) s_motif = IDLE_MOTIF_CURIOUS_CHECK;
        else if (r < 0.90f) s_motif = IDLE_MOTIF_CROSS_SCAN;
        else if (r < 0.95f) s_motif = IDLE_MOTIF_WANDER;
        else                s_motif = IDLE_MOTIF_LINE_BLINK;
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
        case IDLE_MOTIF_SIDE_PEEK:
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
                    do_axis_glance(lateral_micro(s_motif_sign), 0.0f,
                                   rand_interval(GLANCE_HOLD_MICRO_MIN_MS,
                                                 GLANCE_HOLD_MICRO_RNG_MS));
                    break;
                default:
                    finish_idle_motif();
                    break;
            }
            break;

        case IDLE_MOTIF_VERTICAL_SCAN:
            switch (s_motif_step++) {
                case 0:
                    do_axis_glance(0.0f, vertical_peek(-1.0f),
                                   rand_interval(GLANCE_HOLD_PEEK_MIN_MS,
                                                 GLANCE_HOLD_PEEK_RNG_MS));
                    break;
                case 1:
                    do_center_pause();
                    break;
                case 2:
                    do_axis_glance(0.0f, vertical_micro(1.0f),
                                   rand_interval(GLANCE_HOLD_MICRO_MIN_MS,
                                                 GLANCE_HOLD_MICRO_RNG_MS));
                    break;
                default:
                    finish_idle_motif();
                    break;
            }
            break;

        case IDLE_MOTIF_CURIOUS_CHECK:
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

        case IDLE_MOTIF_LINE_BLINK:
            switch (s_motif_step++) {
                case 0:
                    gaze_service_set_target(0.0f, 0.0f);
                    expression_play(NB_EXPR_SLEEPY,
                                    (float)rand_interval(LINE_BLINK_MIN_MS,
                                                         LINE_BLINK_RANGE_MS),
                                    LINE_BLINK_TRANS_MS);
                    schedule_inner_step();
                    break;
                default:
                    finish_idle_motif();
                    break;
            }
            break;

        /*
         * CROSS_SCAN — lateral peek → cima → lateral oposto → centro.
         * Usa peek no primeiro e último, micro no vertical (mais natural).
         */
        case IDLE_MOTIF_CROSS_SCAN:
            switch (s_motif_step++) {
                case 0:
                    do_axis_glance(lateral_peek(s_motif_sign), 0.0f,
                                   rand_interval(GLANCE_HOLD_PEEK_MIN_MS,
                                                 GLANCE_HOLD_PEEK_RNG_MS));
                    break;
                case 1:
                    do_axis_glance(0.0f, vertical_micro(-1.0f),
                                   rand_interval(GLANCE_HOLD_MICRO_MIN_MS,
                                                 GLANCE_HOLD_MICRO_RNG_MS));
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
         * HEAD_TILT_HOLD — postura assimétrica vertical sustentada.
         * Aplica overlay (dy_l, dy_r) com sinais opostos: simula um leve roll
         * da cabeça. Mantém durante 5–15s; não bloqueia outros mecanismos
         * (blink, drift seguem ativos). Ao terminar, limpa o overlay.
         * Ver docs/IDLE_REFERENCE.md §3 (HEAD_TILT_HOLD).
         */
        case IDLE_MOTIF_HEAD_TILT_HOLD:
            switch (s_motif_step++) {
                case 0: {
                    float mag = TILT_HOLD_DY_MIN + rand01() * TILT_HOLD_DY_RNG;
                    /* sign +1: olho L mais baixo (cabeça inclinada para a esquerda do robô)
                     *      -1: olho R mais baixo (cabeça inclinada para a direita)        */
                    float dy_l =  mag * s_motif_sign;
                    float dy_r = -mag * s_motif_sign;
                    expression_service_set_idle_overlay(dy_l, dy_r, 0.0f, 0.0f);
                    s_next_glance_ms = rand_interval(TILT_HOLD_DUR_MIN_MS,
                                                     TILT_HOLD_DUR_RNG_MS);
                    ESP_LOGD(TAG, "tilt_hold dy=(%.2f, %.2f) dur=%lums",
                             dy_l, dy_r, (unsigned long)s_next_glance_ms);
                    break;
                }
                case 1:
                    /* Limpa overlay com retorno suave ao baseline (a transição
                     * é feita pelo render que lê s_idle_dy_* atomicamente; sem
                     * easing — o efeito é discreto, não há flick perceptível). */
                    expression_service_set_idle_overlay(0.0f, 0.0f, 0.0f, 0.0f);
                    schedule_inner_step();
                    break;
                default:
                    finish_idle_motif();
                    break;
            }
            break;

        /*
         * LOOK_DOWN_BLINK — gaze↓, blink-bar, hold ~600ms, blink-bar, return.
         * Sequência composta observada no vídeo (t=18s–20s).
         * Ver docs/IDLE_REFERENCE.md §1.3 e §3.
         */
        case IDLE_MOTIF_LOOK_DOWN_BLINK:
            switch (s_motif_step++) {
                case 0:
                    /* Gaze para baixo + primeiro blink-bar imediato. */
                    gaze_service_glance(0.0f, LDB_GAZE_Y, LDB_GAZE_HOLD_MS);
                    expression_play(NB_EXPR_SLEEPY, (float)LDB_BLINK_DUR_MS,
                                    LDB_BLINK_TRANS_MS);
                    s_next_glance_ms = rand_interval(LDB_HOLD_BETWEEN_MIN_MS,
                                                     LDB_HOLD_BETWEEN_RNG_MS);
                    ESP_LOGD(TAG, "look_down_blink: gaze↓ + blink1");
                    break;
                case 1:
                    /* Segundo blink-bar (gaze ainda baixo via glance hold). */
                    expression_play(NB_EXPR_SLEEPY, (float)LDB_BLINK_DUR_MS,
                                    LDB_BLINK_TRANS_MS);
                    schedule_inner_step();
                    ESP_LOGD(TAG, "look_down_blink: blink2");
                    break;
                /* O retorno do gaze ao âncora (0,0) acontece automaticamente
                 * quando o glance hold expira, sem step explícito aqui.    */
                default:
                    finish_idle_motif();
                    break;
            }
            break;

        /*
         * CURIOUS_TILT — versão sustentada do CURIOUS_CHECK.
         * Toca a expressão CURIOUS (já assimétrica em NB_EXPRESSIONS) por
         * 3.5–5s. O blink Poisson interno do expression_service continua
         * ativo durante o hold — replicando o blink dentro de uma expressão
         * curiosa observado no vídeo (t=23.2s).
         * Ver docs/IDLE_REFERENCE.md §1.4.
         */
        case IDLE_MOTIF_CURIOUS_TILT:
            switch (s_motif_step++) {
                case 0: {
                    uint32_t dur = rand_interval(CURIOUS_TILT_DUR_MIN_MS,
                                                 CURIOUS_TILT_DUR_RNG_MS);
                    expression_play(NB_EXPR_CURIOUS, (float)dur,
                                    CURIOUS_TILT_TRANS_MS);
                    s_next_glance_ms = dur + (uint32_t)CURIOUS_TILT_TRANS_MS;
                    ESP_LOGD(TAG, "curious_tilt dur=%lums", (unsigned long)dur);
                    break;
                }
                default:
                    finish_idle_motif();
                    break;
            }
            break;

        /*
         * BI_LATERAL — peek para um lado, pausa central, peek para o outro.
         * Cobre a percepção de "olha dos dois lados" — o movimento mais
         * reconhecível de curiosidade passiva num robô companion.
         */
        case IDLE_MOTIF_BI_LATERAL:
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
         * VERT_SWEEP — baixo → centro → cima (ou cima → centro → baixo).
         * s_motif_sign=-1: começa para cima; s_motif_sign=+1: começa para baixo.
         * Cobre o "baixo meio cima e vice versa" pedido pelo usuário.
         */
        case IDLE_MOTIF_VERT_SWEEP:
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
         * WANDER — sequência de 3 direções distintas sem padrão fixo:
         * lateral peek → vertical peek oposto à altura atual → lateral oposto.
         * Dá a impressão de varredura exploratória casual.
         */
        case IDLE_MOTIF_WANDER:
            switch (s_motif_step++) {
                case 0:
                    do_axis_glance(lateral_peek(s_motif_sign), 0.0f,
                                   rand_interval(GLANCE_HOLD_PEEK_MIN_MS,
                                                 GLANCE_HOLD_PEEK_RNG_MS));
                    break;
                case 1:
                    /* Vertical puro — direção aleatória via rand_sign() interno */
                    do_axis_glance(0.0f, vertical_peek(rand01() < 0.5f ? -1.0f : 1.0f),
                                   rand_interval(GLANCE_HOLD_MICRO_MIN_MS,
                                                 GLANCE_HOLD_MICRO_RNG_MS));
                    break;
                case 2:
                    do_axis_glance(lateral_micro(-s_motif_sign), 0.0f,
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
    /* Limpa overlay assimetrico — se HEAD_TILT_HOLD ou CURIOUS_TILT estavam
     * em curso, garante que o baseline IDLE volte limpo (regra: toda
     * entrada em IDLE limpa expressao, gaze, postura e overlays). */
    expression_service_set_idle_overlay(0.0f, 0.0f, 0.0f, 0.0f);
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
                    expression_play(NB_EXPR_SLEEPY, 80.0f, 40.0f);
                    ESP_LOGD(TAG, "involuntary: satisfied blink");
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
