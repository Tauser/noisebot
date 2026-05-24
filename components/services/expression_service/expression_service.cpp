/*
 * expression_service.cpp — Implementação do expression_service (modelo EMO)
 *
 * C++ obrigatório: usa LGFX_Sprite diretamente para desenhar a face.
 * API pública em extern "C" para compatibilidade com C.
 *
 * Arquitetura de desenho:
 *   - Layer registrada no render_service com z_order = 10.
 *   - A cada frame (30fps), atualiza interpolação, blink e redesenha.
 *   - Canvas já limpo (TFT_BLACK) pelo render_service antes de cada layer.
 *   - Boca: overlay visual transitório controlado por estado de fala.
 *   - Sobrancelhas: fora deste módulo — peças ocasionais de Layer 5+.
 *
 * Renderer dos olhos (estilo EMO):
 *   - Quadrilátero com cantos independentes + squint + curvatura de borda.
 *   - Varredura coluna-a-coluna para tratar corner offsets assimétricos.
 *   - Sem pupila. Expressão vem inteiramente da geometria do shape.
 *
 * Thread safety:
 *   - s_new_target_pending protegido por mutex (trylock no render callback,
 *     bloqueante em expression_service_set).
 */

#include "expression_service.h"
#include "render_service.h"
#include "ui_overlay_service.h"
#include "led_service.h"
#include "display_lgfx_config.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cmath>

#define TAG "nb_expr"

static void sync_led_mood(nb_expression_t expr)
{
    switch (expr) {
        case NB_EXPR_NEUTRAL:
            led_mood_set(NB_LED_COLOR_BLACK, false);
            break;
        case NB_EXPR_HAPPY:
            led_mood_set(NB_LED_AQUA_HAPPY, true);
            break;
        case NB_EXPR_CURIOUS:
            led_mood_set(NB_LED_CYAN_VIVID, true);
            break;
        case NB_EXPR_SLEEPY:
            led_mood_set(NB_LED_SLEEP_BLUE, true);
            break;
        case NB_EXPR_FOCUSED:
            led_mood_set(NB_LED_FOCUS_BLUE, true);
            break;
        case NB_EXPR_SUSPICIOUS:
        case NB_EXPR_SAD:
            led_mood_set(NB_LED_PURPLE_DIM, true);
            break;
        case NB_EXPR_SURPRISED:
            led_mood_set(NB_LED_SURPRISE_SKY, true);
            break;
        case NB_EXPR_ALARMED:
        case NB_EXPR_ANGRY:
            led_mood_set(NB_LED_RED, true);
            break;
        default:
            led_mood_set(NB_LED_COLOR_BLACK, false);
            break;
    }
}

/* ── Layout da face (pixels) ─────────────────────────────────────────────── */
/*
 * Display 320×240 landscape.
 *
 *   +──────────320px──────────+
 *   |                         | ^
 *   |   [L_eye]   [R_eye]     | 240px
 *   |                         | v
 *   +─────────────────────────+
 *
 * Left eye center:  (96, 122)   Right eye center: (224, 122)
 * Eye half-width:   46px        Max eye half-height: 32px (open=1.0)
 * Inter-eye gap:    36px        Margins: 50px each side
 */

static constexpr int16_t BASE_L_CX    = 96;
static constexpr int16_t BASE_R_CX    = 224;
static constexpr int16_t EYE_CY_BASE  = 122;

static constexpr float HW_F           = 46.0f;   /* half-width em pixels  */
static constexpr float MAX_HH_F       = 46.0f;   /* half-height máxima    */
static constexpr int16_t HW_I         = 46;

static constexpr float Y_TRAVEL_PX    = 32.0f;   /* pixels para y=±1      */
static constexpr float X_OFF_TRAVEL   = 18.0f;   /* pixels para x_off=±1  */
static constexpr float MAX_CURVE_PX   = 10.0f;   /* pixels de curvatura máx */
static constexpr float GAZE_X_TRAVEL_PX = 14.0f; /* pixels de gaze horizontal */
static constexpr float GAZE_Y_MAX        = 0.55f; /* travel vertical permitido (~18px) */

/* Perspectiva lateral: olho do lado do gaze estreita levemente.
 * PERSP_OPEN_FACTOR: redução relativa de open (14% no gaze máximo).
 * PERSP_SQUINT_ADD:  squint adicionado ao olho near (8% no gaze máximo).
 * GAZE_X_MAX:        range máximo de gaze_x (igual a GAZE_MAX em gaze_service). */
static constexpr float GAZE_PERSP_OPEN_FACTOR = 0.14f;
static constexpr float GAZE_PERSP_SQUINT_ADD  = 0.08f;
static constexpr float GAZE_X_MAX             = 0.65f;

/* Pose de fala: durante RESPONDING a composição fica fixa para manter
 * distância previsível entre olhos, boca e balão de texto. */
static constexpr float   SPEAKING_EYE_Y_NORM  = -0.55f;
static constexpr float   SPEAKING_EYE_OPEN    = 0.70f;

/* Boca de fala: pequena, centralizada abaixo dos olhos, ativada durante
 * RESPONDING. Fica acima da cauda do balão inferior. */
static constexpr int16_t MOUTH_CX             = 160;
static constexpr int16_t MOUTH_CY             = 166;
static constexpr int16_t MOUTH_MIN_W          = 38;
static constexpr int16_t MOUTH_MAX_W          = 58;
static constexpr int16_t MOUTH_MIN_H          = 3;
static constexpr int16_t MOUTH_MAX_H          = 13;
static constexpr int16_t MOUTH_EYE_GAP_PX     = 10;
static constexpr int16_t MOUTH_MAX_BOTTOM_Y   = 174;
static constexpr int64_t MOUTH_PERIOD_US      = 320000LL;

/* ── Blink ───────────────────────────────────────────────────────────────── */

/* Tuning calibrado contra o vídeo idle do EMO (re-análise por olho separado).
 * Blinks isolados ocorrem em ~7-10s, mas blinks compostos (look-down + double)
 * e blinks dentro de expressões sustentadas (CURIOUS_TILT) também acontecem.
 * Mantemos média moderada — a riqueza vem dos motifs em idle_service.
 * Ver docs/IDLE_REFERENCE.md §1.3 e §4.1.
 */
static constexpr float   BLINK_MEAN_MS  = 5000.0f;   /* era 4200; testado 7500 (alto demais) */
static constexpr float   BLINK_MIN_MS   = 1800.0f;   /* era 1600                              */
static constexpr int64_t BLINK_CLOSE_US = 55000LL;
static constexpr int64_t BLINK_HOLD_US  = 25000LL;
static constexpr int64_t BLINK_OPEN_US  = 80000LL;

/* Chance de assimetria (valor < 256 = probabilidade / 256). */
static constexpr uint32_t BLINK_ASYM_THRESH = 52u;   /* ~20% */
static constexpr int64_t  BLINK_ASYM_MIN_US = 20000LL;
static constexpr int64_t  BLINK_ASYM_RANGE  = 80000LL;

/* Blink bar EMO: acima deste blink_ph, os dois olhos são substituídos por uma
 * barra única que abrange ambos. Threshold 0.75 → barra visível por ~2 frames. */
static constexpr float   BLINK_BAR_PH_THRESH = 0.75f;
static constexpr int16_t BLINK_BAR_EXTRA_HW  = 3;    /* px de padding além das bordas externas dos olhos */

/* ── Dirty rect conservador da área da face ──────────────────────────────── */
/*
 * Rect fixo que cobre TODOS os pixels possíveis dos olhos em qualquer frame:
 *   gaze shift máximo  = GAZE_MAX(0.65) × GAZE_X_TRAVEL_PX(14) ≈ 9px
 *   x_off máximo       = X_OFF_TRAVEL = 18px
 *   blink bar padding  = BLINK_BAR_EXTRA_HW = 3px
 *   y travel + abertura = Y_TRAVEL_PX + MAX_HH_F + 2px de margem
 *
 * Ser fixo é essencial: como a posição dos olhos muda a cada frame (gaze
 * drift), o rect do frame anterior precisa estar contido no rect atual para
 * que pixels residuais no display sejam apagados pelo canvas limpo.
 * Cobre ~45% do display (< FULL_PUSH_THRESHOLD 85%) → push parcial row-by-row.
 */
static constexpr int GAZE_X_MARGIN  = (int)GAZE_X_TRAVEL_PX;
static constexpr int FACE_DIRTY_X0 = (int)BASE_L_CX  - (int)HW_I - (int)X_OFF_TRAVEL - GAZE_X_MARGIN - (int)BLINK_BAR_EXTRA_HW;
static constexpr int FACE_DIRTY_X1_RAW = (int)BASE_R_CX + (int)HW_I + (int)X_OFF_TRAVEL
                                       + GAZE_X_MARGIN + (int)BLINK_BAR_EXTRA_HW + 20;
static constexpr int FACE_DIRTY_X1 = FACE_DIRTY_X1_RAW > 320 ? 320 : FACE_DIRTY_X1_RAW;
/* ROT_MARGIN: folga extra para olhos rotacionados em até 30° (96×96 sprite → diagonal ~68px vs 46px). */
static constexpr int ROT_MARGIN    = 22;
static constexpr int FACE_DIRTY_Y0 = (int)EYE_CY_BASE - (int)MAX_HH_F - (int)Y_TRAVEL_PX - (int)MAX_CURVE_PX - ROT_MARGIN;
static constexpr int FACE_DIRTY_Y1 = (int)EYE_CY_BASE + (int)MAX_HH_F + (int)Y_TRAVEL_PX + (int)MAX_CURVE_PX + ROT_MARGIN + 8;

/* ── Sprite de face combinado (rotação) ──────────────────────────────────── */
/* Sprite único 320×96px cobrindo os dois olhos. A rotação acontece ao redor
 * do centro da face (x=160), não do centro de cada olho — os dois giram
 * como uma unidade (head tilt real). Olhos desenhados nas posições de tela
 * relativas ao sprite (stride = largura do display). */
static constexpr int   SPR_W    = 320;
static constexpr int   SPR_H    = 96;
static constexpr float SPR_FCX  = 160.0f;  /* centro de face em X (= centro do sprite) */
static constexpr float SPR_CYF  = 48.0f;   /* centro vertical do sprite */
static LGFX_Sprite s_face_spr;
static bool        s_face_spr_ready = false;

typedef enum {
    BLINK_IDLE,
    BLINK_CLOSING,
    BLINK_CLOSED,
    BLINK_OPENING,
} blink_state_t;

typedef struct {
    blink_state_t state;
    float         phase;   /* [0..1], 1 = totalmente fechado */
    int64_t       t0;
} nb_blink_eye_t;

/* ── Constantes de timing ────────────────────────────────────────────────── */

/** Duração estimada de um frame a 30fps. */
static constexpr float FRAME_MS = 33.3f;

/* ── Fila de expressões temporárias (play queue) ─────────────────────────── */

#define PLAY_QUEUE_CAP  4

struct play_item_t {
    nb_expression_t expr;
    float           duration_ms;
    float           trans_ms;
};

typedef enum {
    PLAY_STATE_IDLE,    /* sem play ativo — base expression vigente */
    PLAY_STATE_HOLD,    /* contando duração do play                 */
    PLAY_STATE_OUT,     /* retornando à base após play              */
} play_state_t;

/* ── Estado interno ──────────────────────────────────────────────────────── */

static bool               s_initialized      = false;
static SemaphoreHandle_t  s_set_mutex        = NULL;

static nb_face_state_t    s_current          = {};
static nb_face_state_t    s_target           = {};
static nb_face_state_t    s_from             = {};
static float              s_trans_total_ms   = 0.0f;
static float              s_trans_elapsed_ms = 0.0f;

/* Base expression (última definida por expression_service_set) */
static nb_expression_t    s_base_expr          = NB_EXPR_NEUTRAL;

/* Pending base update (escrito de qualquer task, consumido no render Core 1) */
static volatile bool      s_new_target_pending = false;
static nb_face_state_t    s_pending_target     = {};
static float              s_pending_trans_ms   = 0.0f;
static nb_expression_t    s_pending_base_expr  = NB_EXPR_NEUTRAL;

/* Play queue (protegida por s_set_mutex) */
static play_item_t        s_play_queue[PLAY_QUEUE_CAP];
static int                s_play_head          = 0;
static int                s_play_count         = 0;

/* Play state machine (somente render_task Core 1) */
static play_state_t       s_play_state         = PLAY_STATE_IDLE;
static float              s_play_elapsed_ms    = 0.0f;  /* elapsed em HOLD   */
static float              s_play_dur_ms        = 0.0f;  /* duração do play   */
static float              s_play_tr_ms         = 0.0f;  /* duração trans     */
static float              s_play_ret_ms        = 0.0f;  /* elapsed em OUT    */
static nb_expression_t    s_active_expr        = NB_EXPR_NEUTRAL;

/* Dois olhos independentes para blink assimétrico */
static nb_blink_eye_t     s_blink[2]          = {};
static int64_t            s_next_blink_us     = 0;

/* Duplo blink: ~30% dos blinks disparam um segundo blink 400–1000ms depois.
 * Calibrado contra vídeo idle do EMO (2 double em 3 eventos / 30s — ver
 * docs/IDLE_REFERENCE.md §1.3 e §3.2). */
static bool               s_double_blink_pending = false;
static int64_t            s_double_blink_us      = 0;
static constexpr uint32_t DOUBLE_BLINK_THRESH    = 77u;   /* /256 ≈ 30% (era 31 ≈12%) */
static constexpr int64_t  DOUBLE_BLINK_MIN_US    = 400000LL; /* era 180000 */
static constexpr int64_t  DOUBLE_BLINK_RNG_US    = 600000LL; /* gap total 400–1000ms (era 180–380) */

/*
 * Gaze offset — escrito por gaze_render_cb (z=5) e lido por este callback
 * (z=10), ambos no mesmo Core 1 render_task, sequencialmente no mesmo frame.
 * Não são volatile: volatile não provê barrier cross-core no Xtensa, e o
 * acesso é single-core. Nenhuma outra task deve chamar set_gaze diretamente.
 */
static float              s_gaze_x            = 0.0f;
static float              s_gaze_y            = 0.0f;

/* Overlay assimétrico de IDLE (head_tilt, curious_tilt, etc).
 * Escrito pela behavior_task (idle_service); lido no render_task.
 * Float 32-bit é atômico em ESP32-S3 single-word load/store. */
static volatile float     s_idle_dy_l         = 0.0f;
static volatile float     s_idle_dy_r         = 0.0f;
static volatile float     s_idle_dopen_l      = 0.0f;
static volatile float     s_idle_dopen_r      = 0.0f;
static volatile float     s_idle_rot_l        = 0.0f;   /* rotação em graus, olho esq */
static volatile float     s_idle_rot_r        = 0.0f;   /* rotação em graus, olho dir */

/* Pixels de deslocamento horizontal por unidade de gaze_x (translation bilateral). */

typedef struct {
    bool     active;
    uint8_t  intensity;
    uint32_t duration_ms;
    int64_t  start_us;
} blush_overlay_t;

typedef struct {
    bool     active;
    uint32_t duration_ms;
    int64_t  start_us;
} heart_overlay_t;

static blush_overlay_t    s_blush_overlay      = {};
static heart_overlay_t    s_heart_overlay      = {};
static volatile bool      s_breath_enabled     = false;
static volatile bool      s_blink_enabled      = true;
static volatile bool      s_sleep_anim_enabled = false;
static volatile bool      s_speaking_mouth_enabled = false;
static bool               s_blink_prev_enabled = true;
static int64_t            s_sleep_anim_start_us = 0;
static int64_t            s_speaking_mouth_start_us = 0;

static constexpr float BREATH_PERIOD_MS    = 5200.0f;
static constexpr float BREATH_AMP          = 0.045f;
static constexpr float SLEEP_EYE_PERIOD_MS = 6200.0f;   /* respiração calma dos olhos */
static constexpr float NB_PI_F             = 3.14159265358979323846f;

static constexpr float SLEEP_STAGE_DROWSY_END_MS   = 2500.0f;
static constexpr float SLEEP_STAGE_RESIST_END_MS   = 7600.0f;
static constexpr float SLEEP_STAGE_CLOSE_END_MS    = 9000.0f;
static constexpr float SLEEP_STAGE_REOPEN_END_MS   = 13000.0f;

static constexpr int   WAKE_SEQ_COUNT              = 20;
static constexpr float WAKE_SEQ_FRAME_MS           = 130.0f;

static volatile bool   s_wake_seq_pending          = false;
static bool            s_wake_seq_active           = false;
static float           s_wake_seq_elapsed_ms       = 0.0f;

/* ── 6 Expressões base ───────────────────────────────────────────────────── */
/*
 * { tl_l, tr_l, bl_l, br_l,   (left eye corners)
 *   tl_r, tr_r, bl_r, br_r,   (right eye corners)
 *   open_l, open_r,
 *   y_l, y_r,
 *   x_off,
 *   rt_top, rb_bot,
 *   cv_top, cv_bot,
 *   color,
 *   squint_l, squint_r }
 *
 * Convenção de corners top: positivo = canto DESCE (fecha o topo daquele lado)
 * Convenção de corners bot: positivo = canto SOBE  (fecha o fundo daquele lado)
 *
 * Exemplo:
 *   tr_l=0.50, tl_r=0.50 → cantos internos do topo descem → expressão V no topo
 *                           (olhar concentrated/suspeito)
 *   bl_l=0.70, br_l=0.70,
 *   bl_r=0.70, br_r=0.70 → cantos inferiores sobem → arco na base dos olhos
 *                           (olhos sorrindo)
 */

extern "C" const nb_face_state_t NB_EXPRESSIONS[NB_EXPR_COUNT] = {

    /* NEUTRAL — quadrado limpo, leve curvatura no topo: leitura calma e presente */

    {
        .tl_l=0.00f,.tr_l=0.00f,.bl_l=0.00f,.br_l=0.00f,
        .tl_r=0.00f,.tr_r=0.00f,.bl_r=0.00f,.br_r=0.00f,
        .open_l=0.88f, .open_r=0.88f,
        .y_l=0.00f,    .y_r=0.00f,
        .x_off=0.60f,
        .rt_top=0.64f, .rb_bot=0.64f,
        .cv_top=0.00f, .cv_bot=0.00f,
        .color=TFT_WHITE,
        .squint_l=0.00f, .squint_r=0.00f,
    },



    /* HAPPY — arco forte na base, squint suave: olhos em meia-lua */
    {
        .tl_l=0.00f,.tr_l=0.00f,.bl_l=0.72f,.br_l=0.72f,
        .tl_r=0.00f,.tr_r=0.00f,.bl_r=0.72f,.br_r=0.72f,
        .open_l=0.41f, .open_r=0.41f,
        .y_l=0.00f,    .y_r=0.00f,
        .x_off=0.60f,
        .rt_top=0.27f, .rb_bot=0.52f,
        .cv_top=1.00f, .cv_bot=-1.00f,
        .color=TFT_WHITE,
        .squint_l=0.22f, .squint_r=0.22f,
    },

    /* CURIOUS — olho direito mais aberto e mais alto, curvatura no topo forte.
     * y_r=0: com bottom-alignment, open_r=1.00 vs open_l=0.82 já eleva o topo
     * direito ~17px acima do esquerdo sem desalinhar as bases. */
    {
        .tl_l=0.00f,.tr_l=0.00f,.bl_l=0.00f,.br_l=0.00f,
        .tl_r=0.19f,.tr_r=0.00f,.bl_r=0.00f,.br_r=0.00f,
        .open_l=0.82f, .open_r=0.96f,
        .y_l=0.00f,    .y_r=0.00f,
        .x_off=0.60f,
        .rt_top=0.64f, .rb_bot=0.64f,
        .cv_top=0.00f, .cv_bot=0.00f,
        .color=TFT_WHITE,
        .squint_l=0.00f, .squint_r=0.00f,
    },

    /* SLEEPY — abertura baixa, squint pesado, olhos centrados para o balão Zzz */
    {
        .tl_l=0.00f,.tr_l=0.00f,.bl_l=0.00f,.br_l=0.00f,
        .tl_r=0.00f,.tr_r=0.00f,.bl_r=0.00f,.br_r=0.00f,
        .open_l=0.14f, .open_r=0.14f,
        .y_l=-0.55f,   .y_r=-0.55f,
        .x_off=0.60f,
        .rt_top=0.19f, .rb_bot=1.00f,
        .cv_top=-0.38f, .cv_bot=0.45f,
        .color=TFT_WHITE,
        .squint_l=0.51f, .squint_r=0.51f,
    },


    /* FOCUSED — cantos internos do topo descidos, squint leve: olhar concentrado */
    {
        .tl_l=0.00f,.tr_l=0.30f,.bl_l=0.00f,.br_l=0.00f,
        .tl_r=0.30f,.tr_r=0.00f,.bl_r=0.00f,.br_r=0.00f,
        .open_l=0.80f, .open_r=0.80f,
        .y_l=0.00f,    .y_r=0.00f,
        .x_off=0.60f,
        .rt_top=0.64f, .rb_bot=0.64f,
        .cv_top=0.30f, .cv_bot=0.05f,
        .color=TFT_WHITE,
        .squint_l=0.10f, .squint_r=0.10f,
    },

    /* SUSPICIOUS — cantos internos fortemente descidos, squint médio: V agressivo */
    {
        .tl_l=0.00f,.tr_l=0.38f,.bl_l=0.00f,.br_l=0.00f,
        .tl_r=0.38f,.tr_r=0.00f,.bl_r=0.00f,.br_r=0.00f,
        .open_l=0.86f, .open_r=0.86f,
        .y_l=0.10f,    .y_r=0.10f,
        .x_off=0.60f,
        .rt_top=0.64f, .rb_bot=0.64f,
        .cv_top=-0.44f, .cv_bot=0.05f,
        .color=TFT_WHITE,
        .squint_l=0.38f, .squint_r=0.38f,
    },

    /* SURPRISED — olhos arregalados: open > 1.0, cantos abertos, sem squint */
    {
        .tl_l=0.00f,.tr_l=0.00f,.bl_l=0.00f,.br_l=0.00f,
        .tl_r=0.00f,.tr_r=0.00f,.bl_r=0.00f,.br_r=0.00f,
        .open_l=1.00f, .open_r=1.00f,
        .y_l=0.00f,    .y_r=0.00f,
        .x_off=0.60f,
        .rt_top=0.64f, .rb_bot=0.64f,
        .cv_top=0.00f, .cv_bot=0.00f,
        .color=TFT_WHITE,
        .squint_l=0.00f, .squint_r=0.00f,
    },

    /* SAD — cantos externos do topo descidos (droopy), olhos abaixados, leve squint */
    /*
     * tl_l / tr_r (outer-top) = 0.55 → borda exterior-topo desce em ambos os olhos
     * br_l / bl_r             = 0.20 → cantos internos do fundo sobem levemente
     * cv_top negativo          = topo côncavo (olhar pesado)
     * Resultado: V invertido no topo — inclinação externa-para-baixo = tristeza clássica
     */
    {
        .tl_l=0.70f,.tr_l=0.13f,.bl_l=0.00f,.br_l=0.44f,
        .tl_r=0.00f,.tr_r=0.70f,.bl_r=0.44f,.br_r=0.00f,
        .open_l=0.68f, .open_r=0.68f,
        .y_l=0.20f,    .y_r=0.20f,
        .x_off=0.60f,
        .rt_top=0.64f, .rb_bot=0.64f,
        .cv_top=-0.16f, .cv_bot=0.08f,
        .color=TFT_WHITE,
        .squint_l=0.08f, .squint_r=0.08f,
    },

    /* ALARMED — olhos largos com V de tensão interno, levantados, sem squint */
    /*
     * tr_l / tl_r (inner-top) = 0.28 → cantos internos do topo fecham = tensão
     * open 0.95                = quase arregalado, distinto de SURPRISED (1.10)
     * y negativo               = olhos para cima (reação de susto/alerta)
     * x_off leve               = leve convergência (estado de alerta)
     * cv_top alto              = topo fortemente convexo (olho aberto com tensão)
     */
    {
        .tl_l=0.00f,.tr_l=0.28f,.bl_l=0.00f,.br_l=0.00f,
        .tl_r=0.28f,.tr_r=0.00f,.bl_r=0.00f,.br_r=0.00f,
        .open_l=0.88f, .open_r=0.88f,
        .y_l=-0.18f,    .y_r=-0.18f,
        .x_off=0.60f,
        .rt_top=0.64f, .rb_bot=0.64f,
        .cv_top=0.55f, .cv_bot=0.10f,
        .color=TFT_WHITE,
        .squint_l=0.00f, .squint_r=0.00f,
    },

    /* ANGRY */
    {
        .tl_l=0.00f,.tr_l=0.88f,.bl_l=0.93f,.br_l=0.60f,
        .tl_r=0.88f,.tr_r=0.00f,.bl_r=0.60f,.br_r=0.93f,
        .open_l=0.82f, .open_r=0.82f,
        .y_l=0.00f,    .y_r=0.00f,
        .x_off=0.60f,
        .rt_top=0.26f, .rb_bot=0.14f,
        .cv_top=0.06f, .cv_bot=-0.10f,
        .color=TFT_WHITE,
        .squint_l=0.00f, .squint_r=0.00f,
    },
};

#define WAKE_FACE(tll, trl, bll, brl, tlr, trr, blr, brr, op_l, op_r, yy_l, yy_r, xof, rt, rb, cvt, cvb, sql, sqr) \
    { .tl_l=(tll),.tr_l=(trl),.bl_l=(bll),.br_l=(brl), \
      .tl_r=(tlr),.tr_r=(trr),.bl_r=(blr),.br_r=(brr), \
      .open_l=(op_l),.open_r=(op_r),.y_l=(yy_l),.y_r=(yy_r), \
      .x_off=(xof),.rt_top=(rt),.rb_bot=(rb),.cv_top=(cvt),.cv_bot=(cvb), \
      .color=TFT_WHITE,.squint_l=(sql),.squint_r=(sqr) }

static const nb_face_state_t k_wake_sequence[WAKE_SEQ_COUNT] = {
    /* 0–3: olhos retangulares acordando devagar. */
    WAKE_FACE(0.00f,0.00f,0.00f,0.00f, 0.00f,0.00f,0.00f,0.00f, 0.62f,0.62f, 0.25f,0.25f, 0.60f, 0.42f,0.58f, 0.00f,0.00f, 0.00f,0.00f),
    WAKE_FACE(0.00f,0.00f,0.00f,0.00f, 0.00f,0.00f,0.00f,0.00f, 0.66f,0.66f, 0.20f,0.20f, 0.60f, 0.46f,0.58f, 0.00f,0.00f, 0.00f,0.00f),
    WAKE_FACE(0.00f,0.00f,0.00f,0.00f, 0.00f,0.00f,0.00f,0.00f, 0.58f,0.58f, 0.08f,0.08f, 0.60f, 0.50f,0.58f, 0.08f,0.00f, 0.00f,0.00f),
    WAKE_FACE(0.00f,0.00f,0.00f,0.00f, 0.00f,0.00f,0.00f,0.00f, 0.56f,0.56f, 0.06f,0.06f, 0.60f, 0.50f,0.58f, 0.06f,0.00f, 0.00f,0.00f),

    /* 4–8: olhos em arco sonolento, sem texto de saudação. */
    WAKE_FACE(0.00f,0.00f,0.82f,0.82f, 0.00f,0.00f,0.82f,0.82f, 0.34f,0.34f, 0.00f,0.00f, 0.60f, 0.24f,0.52f, 0.95f,-1.00f, 0.16f,0.16f),
    WAKE_FACE(0.00f,0.00f,0.86f,0.86f, 0.00f,0.00f,0.86f,0.86f, 0.32f,0.32f,-0.02f,-0.02f, 0.60f, 0.22f,0.50f, 1.00f,-1.00f, 0.18f,0.18f),
    WAKE_FACE(0.00f,0.00f,0.86f,0.86f, 0.00f,0.00f,0.86f,0.86f, 0.31f,0.31f,-0.02f,-0.02f, 0.60f, 0.22f,0.50f, 1.00f,-1.00f, 0.20f,0.20f),
    WAKE_FACE(0.00f,0.00f,0.84f,0.84f, 0.00f,0.00f,0.84f,0.84f, 0.30f,0.30f,-0.01f,-0.01f, 0.60f, 0.22f,0.50f, 0.95f,-1.00f, 0.18f,0.18f),
    WAKE_FACE(0.00f,0.00f,0.82f,0.82f, 0.00f,0.00f,0.82f,0.82f, 0.30f,0.30f, 0.00f,0.00f, 0.60f, 0.24f,0.52f, 0.90f,-1.00f, 0.16f,0.16f),

    /* 9–15: volta progressiva de arco para olhos abertos, sem pico de alerta. */
    WAKE_FACE(0.00f,0.00f,0.76f,0.76f, 0.00f,0.00f,0.76f,0.76f, 0.34f,0.34f, 0.05f,0.05f, 0.60f, 0.28f,0.54f, 0.70f,-0.85f, 0.16f,0.16f),
    WAKE_FACE(0.00f,0.00f,0.52f,0.52f, 0.00f,0.00f,0.52f,0.52f, 0.40f,0.40f, 0.10f,0.10f, 0.60f, 0.34f,0.56f, 0.45f,-0.52f, 0.12f,0.12f),
    WAKE_FACE(0.00f,0.00f,0.34f,0.34f, 0.00f,0.00f,0.34f,0.34f, 0.48f,0.48f, 0.12f,0.12f, 0.60f, 0.42f,0.58f, 0.25f,-0.25f, 0.08f,0.08f),
    WAKE_FACE(0.00f,0.00f,0.18f,0.18f, 0.00f,0.00f,0.18f,0.18f, 0.58f,0.58f, 0.10f,0.10f, 0.60f, 0.50f,0.60f, 0.12f,-0.10f, 0.04f,0.04f),
    WAKE_FACE(0.00f,0.00f,0.08f,0.08f, 0.00f,0.00f,0.08f,0.08f, 0.66f,0.66f, 0.08f,0.08f, 0.60f, 0.56f,0.62f, 0.04f,0.00f, 0.02f,0.02f),
    WAKE_FACE(0.00f,0.00f,0.00f,0.00f, 0.00f,0.00f,0.00f,0.00f, 0.74f,0.74f, 0.05f,0.05f, 0.60f, 0.60f,0.64f, 0.00f,0.00f, 0.00f,0.00f),
    WAKE_FACE(0.00f,0.00f,0.00f,0.00f, 0.00f,0.00f,0.00f,0.00f, 0.80f,0.80f, 0.03f,0.03f, 0.60f, 0.62f,0.64f, 0.00f,0.00f, 0.00f,0.00f),

    /* 16–19: estabilização pesada para NEUTRAL. */
    WAKE_FACE(0.00f,0.00f,0.00f,0.00f, 0.00f,0.00f,0.00f,0.00f, 0.84f,0.84f, 0.02f,0.02f, 0.60f, 0.62f,0.64f, 0.00f,0.00f, 0.00f,0.00f),
    WAKE_FACE(0.00f,0.00f,0.00f,0.00f, 0.00f,0.00f,0.00f,0.00f, 0.86f,0.86f, 0.01f,0.01f, 0.60f, 0.63f,0.64f, 0.00f,0.00f, 0.00f,0.00f),
    WAKE_FACE(0.00f,0.00f,0.00f,0.00f, 0.00f,0.00f,0.00f,0.00f, 0.88f,0.88f, 0.00f,0.00f, 0.60f, 0.64f,0.64f, 0.00f,0.00f, 0.00f,0.00f),
    WAKE_FACE(0.00f,0.00f,0.00f,0.00f, 0.00f,0.00f,0.00f,0.00f, 0.88f,0.88f, 0.00f,0.00f, 0.60f, 0.64f,0.64f, 0.00f,0.00f, 0.00f,0.00f),
};

#undef WAKE_FACE

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static inline float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static inline float clamp_abs(float v, float max_abs)
{
    if (v > max_abs) return max_abs;
    if (v < -max_abs) return -max_abs;
    return v;
}

static inline float damp_vertical_for_lateral_gaze(float gx, float gy)
{
    /* Só damp quando fortemente lateral (>0.30) — preserva diagonais orgânicas. */
    float absx = fabsf(gx);
    if (absx > 0.30f) {
        float t = (absx - 0.30f) / (GAZE_X_MAX - 0.30f);  /* 0→1 no range extremo */
        return gy * (1.0f - t * 0.55f);                    /* reduz até 45% no extremo */
    }
    return gy;
}

static inline float smooth01(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v * v * (3.0f - 2.0f * v);
}

static void apply_sleep_visual_stage(nb_face_state_t *face, float elapsed_ms,
                                     float *sleep_bob_norm)
{
    const nb_face_state_t neutral = NB_EXPRESSIONS[NB_EXPR_NEUTRAL];
    const nb_face_state_t sleepy  = NB_EXPRESSIONS[NB_EXPR_SLEEPY];

    if (elapsed_ms < SLEEP_STAGE_DROWSY_END_MS) {
        /* 1. Sono chegando: olhos ainda abertos, começando a pesar. */
        float t = smooth01(elapsed_ms / SLEEP_STAGE_DROWSY_END_MS);
        *face = neutral;
        face->open_l = 0.86f - t * 0.12f;
        face->open_r = 0.86f - t * 0.10f;
        face->y_l = t * 0.05f;
        face->y_r = t * 0.04f;
        face->squint_l = t * 0.10f;
        face->squint_r = t * 0.08f;
        face->cv_top = 0.50f - t * 0.16f;
        return;
    }

    if (elapsed_ms < SLEEP_STAGE_RESIST_END_MS) {
        /* 2. Resistência: tenta ficar acordado, com assimetria sonolenta. */
        float t = (elapsed_ms - SLEEP_STAGE_DROWSY_END_MS)
                / (SLEEP_STAGE_RESIST_END_MS - SLEEP_STAGE_DROWSY_END_MS);
        float ease = smooth01(t);
        float wobble = sinf(t * 3.0f * NB_PI_F);
        *face = neutral;
        face->open_l = 0.74f - ease * 0.28f + wobble * 0.035f;
        face->open_r = 0.76f - ease * 0.38f - wobble * 0.050f;
        if (face->open_l < 0.30f) face->open_l = 0.30f;
        if (face->open_r < 0.22f) face->open_r = 0.22f;
        face->y_l = 0.04f + ease * 0.09f;
        face->y_r = 0.02f + ease * 0.13f;
        face->squint_l = 0.10f + ease * 0.20f;
        face->squint_r = 0.14f + ease * 0.26f;
        face->cv_top = 0.34f - ease * 0.28f;
        face->rt_top = 0.30f - ease * 0.09f;
        face->rb_bot = 0.42f + ease * 0.18f;
        return;
    }

    if (elapsed_ms < SLEEP_STAGE_CLOSE_END_MS) {
        /* 3a. Dorme: fecha de vez pela primeira vez. */
        float t = smooth01((elapsed_ms - SLEEP_STAGE_RESIST_END_MS)
                         / (SLEEP_STAGE_CLOSE_END_MS - SLEEP_STAGE_RESIST_END_MS));
        nb_face_state_t from = neutral;
        from.open_l = 0.46f;
        from.open_r = 0.34f;
        from.y_l = 0.13f;
        from.y_r = 0.15f;
        from.squint_l = 0.30f;
        from.squint_r = 0.40f;
        from.rt_top = 0.22f;
        from.rb_bot = 0.60f;
        from.cv_top = 0.06f;
        from.cv_bot = 0.34f;
        nb_face_state_lerp(&from, &sleepy, t, face);
        return;
    }

    if (elapsed_ms < SLEEP_STAGE_REOPEN_END_MS) {
        /* 3b. Tenta abrir um pouco, mas perde a luta e dorme. */
        float t = (elapsed_ms - SLEEP_STAGE_CLOSE_END_MS)
                / (SLEEP_STAGE_REOPEN_END_MS - SLEEP_STAGE_CLOSE_END_MS);
        float lift = sinf(t * NB_PI_F);
        *face = sleepy;
        face->open_l = sleepy.open_l + lift * 0.075f;
        face->open_r = sleepy.open_r + lift * 0.045f;
        face->y_l = sleepy.y_l;
        face->y_r = sleepy.y_r;
        face->squint_l = sleepy.squint_l - lift * 0.08f;
        face->squint_r = sleepy.squint_r - lift * 0.05f;
        return;
    }

    /* 4. Dorme de vez: SLEEPY fixo, só respirando em bloco. */
    *face = sleepy;
    float phase = fmodf(elapsed_ms - SLEEP_STAGE_REOPEN_END_MS,
                        SLEEP_EYE_PERIOD_MS) / SLEEP_EYE_PERIOD_MS;
    float inhale = 0.5f - 0.5f * cosf(phase * 2.0f * NB_PI_F);
    *sleep_bob_norm = -(inhale - 0.5f) * 1.35f / Y_TRAVEL_PX;
}

static inline float lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

/*
 * Blenda color (RGB888) com o fundo preto pelo fator alpha [0..1].
 * Usado para pixels de borda sub-pixel (anti-aliasing).
 * Assume fundo preto — o canvas é limpo para TFT_BLACK antes de cada frame.
 */
static inline uint32_t blend_with_black(uint32_t color, float alpha)
{
    uint8_t r = (uint8_t)((float)((color >> 16) & 0xFFu) * alpha);
    uint8_t g = (uint8_t)((float)((color >> 8)  & 0xFFu) * alpha);
    uint8_t b = (uint8_t)((float)(color          & 0xFFu) * alpha);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline uint32_t blend_color_over_rgb565(uint16_t src, uint32_t color, float alpha)
{
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;

    uint8_t r = (uint8_t)((((uint32_t)(src >> 11) & 0x1Fu) * 255u) / 31u);
    uint8_t g = (uint8_t)((((uint32_t)(src >> 5)  & 0x3Fu) * 255u) / 63u);
    uint8_t b = (uint8_t)(( ((uint32_t)src        & 0x1Fu) * 255u) / 31u);

    float cr = (float)((color >> 16) & 0xFFu);
    float cg = (float)((color >> 8)  & 0xFFu);
    float cb = (float)( color        & 0xFFu);

    r = (uint8_t)((float)r + (cr - (float)r) * alpha);
    g = (uint8_t)((float)g + (cg - (float)g) * alpha);
    b = (uint8_t)((float)b + (cb - (float)b) * alpha);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline void draw_color_alpha_pixel(LGFX_Sprite *spr,
                                          int16_t x, int16_t y,
                                          uint32_t color, float alpha)
{
    if (alpha <= 0.0f) return;
    spr->drawPixel(x, y, blend_color_over_rgb565(spr->readPixel(x, y), color, alpha));
}

static void draw_overlay_line_thick(LGFX_Sprite *spr,
                                    int16_t x0, int16_t y0,
                                    int16_t x1, int16_t y1,
                                    uint32_t color, int16_t radius)
{
    for (int16_t dy = (int16_t)-radius; dy <= radius; ++dy) {
        for (int16_t dx = (int16_t)-radius; dx <= radius; ++dx) {
            if ((dx * dx + dy * dy) > (radius * radius + radius)) continue;
            spr->drawLine((int16_t)(x0 + dx), (int16_t)(y0 + dy),
                          (int16_t)(x1 + dx), (int16_t)(y1 + dy),
                          color);
        }
    }
}

static void draw_overlay_quad_capsule(LGFX_Sprite *spr,
                                      float x0, float y0,
                                      float x1, float y1,
                                      float x2, float y2,
                                      uint32_t color, int16_t radius)
{
    for (int i = 0; i <= 12; ++i) {
        float t = (float)i / 12.0f;
        float u = 1.0f - t;
        float x = u * u * x0 + 2.0f * u * t * x1 + t * t * x2;
        float y = u * u * y0 + 2.0f * u * t * y1 + t * t * y2;
        int16_t px = (int16_t)(x + (x >= 0.0f ? 0.5f : -0.5f));
        int16_t py = (int16_t)(y + (y >= 0.0f ? 0.5f : -0.5f));
        spr->fillCircle(px, py, radius, color);
    }
}

static inline float overlay_alpha(int64_t now_us, int64_t start_us, uint32_t duration_ms)
{
    if (duration_ms == 0U) return 0.0f;
    int64_t elapsed_us = now_us - start_us;
    if (elapsed_us < 0) return 0.0f;
    float elapsed_ms = (float)elapsed_us / 1000.0f;
    if (elapsed_ms >= (float)duration_ms) return 0.0f;

    float t = elapsed_ms / (float)duration_ms;
    if (t < 0.15f) {
        return t / 0.15f;
    }
    if (t > 0.70f) {
        return (1.0f - t) / 0.30f;
    }
    return 1.0f;
}

static void draw_blush_hatch(LGFX_Sprite *spr, int16_t cx, int16_t cy,
                             uint32_t color)
{
    draw_overlay_quad_capsule(spr,
                              (float)(cx - 5), (float)(cy - 8),
                              (float)(cx - 8), (float)(cy - 1),
                              (float)(cx + 4), (float)(cy + 7),
                              color, 4);
}

static void draw_solid_heart(LGFX_Sprite *spr, int16_t cx, int16_t cy,
                             int16_t size, float angle_rad, uint32_t color)
{
    static constexpr int8_t HEART_POLY[][2] = {
        {  0,  15}, { -8,  11}, {-16,   5}, {-21,  -3},
        {-22, -10}, {-19, -17}, {-13, -21}, { -7, -21},
        { -3, -20}, {  0, -16}, {  3, -20}, {  7, -21},
        { 13, -21}, { 19, -17}, { 22, -10}, { 21,  -3},
        { 16,   5}, {  8,  11}
    };
    static constexpr int POINTS = (int)(sizeof(HEART_POLY) / sizeof(HEART_POLY[0]));
    const float scale = (float)size / 22.0f;
    const float c = cosf(angle_rad);
    const float s = sinf(angle_rad);
    float px[POINTS];
    float py[POINTS];
    float min_y = 999.0f;
    float max_y = -999.0f;

    for (int i = 0; i < POINTS; ++i) {
        float x = (float)HEART_POLY[i][0] * scale;
        float y = (float)HEART_POLY[i][1] * scale;
        px[i] = (float)cx + x * c - y * s;
        py[i] = (float)cy + x * s + y * c;
        if (py[i] < min_y) min_y = py[i];
        if (py[i] > max_y) max_y = py[i];
    }

    int16_t top = (int16_t)(min_y - 1.0f);
    int16_t bottom = (int16_t)(max_y + 1.0f);

    for (int16_t y = top; y <= bottom; ++y) {
        float intersections[POINTS];
        int count = 0;
        for (int i = 0; i < POINTS; ++i) {
            int j = (i + 1) % POINTS;
            float x0 = px[i];
            float y0 = py[i];
            float x1 = px[j];
            float y1 = py[j];
            float scan_y = (float)y + 0.5f;
            if ((y0 <= scan_y && y1 > scan_y) || (y1 <= scan_y && y0 > scan_y)) {
                float t = (scan_y - y0) / (y1 - y0);
                if (count < POINTS) {
                    intersections[count++] = x0 + (x1 - x0) * t;
                }
            }
        }
        for (int a = 0; a < count - 1; ++a) {
            for (int b = a + 1; b < count; ++b) {
                if (intersections[b] < intersections[a]) {
                    float tmp = intersections[a];
                    intersections[a] = intersections[b];
                    intersections[b] = tmp;
                }
            }
        }
        for (int i = 0; i + 1 < count; i += 2) {
            int16_t x0 = (int16_t)(intersections[i] + 0.5f);
            int16_t x1 = (int16_t)(intersections[i + 1] + 0.5f);
            spr->drawFastHLine(x0, y, (int32_t)x1 - (int32_t)x0 + 1, color);
        }
    }
}

static void draw_blush_overlay(LGFX_Sprite *spr, int64_t now_us,
                               int16_t left_cx, int16_t right_cx,
                               int16_t left_cy, int16_t right_cy)
{
    if (!s_blush_overlay.active) return;

    if (overlay_alpha(now_us, s_blush_overlay.start_us,
                      s_blush_overlay.duration_ms) <= 0.0f) {
        s_blush_overlay.active = false;
        return;
    }

    uint32_t color = 0xFF7F9Au;

    int16_t lx = (int16_t)(left_cx - HW_I - 12);
    int16_t rx = (int16_t)(right_cx + HW_I + 12);
    int16_t ly = (int16_t)(left_cy + 43);
    int16_t ry = (int16_t)(right_cy + 43);

    draw_blush_hatch(spr, (int16_t)(lx - 10), (int16_t)(ly - 2), color);
    draw_blush_hatch(spr, (int16_t)(lx + 4), ly, color);
    draw_blush_hatch(spr, (int16_t)(lx + 18), (int16_t)(ly + 2), color);

    draw_blush_hatch(spr, (int16_t)(rx - 18), (int16_t)(ry - 2), color);
    draw_blush_hatch(spr, (int16_t)(rx - 4), ry, color);
    draw_blush_hatch(spr, (int16_t)(rx + 10), (int16_t)(ry + 2), color);
}

static void draw_heart_overlay(LGFX_Sprite *spr, int64_t now_us,
                               int16_t right_cx, int16_t eye_cy)
{
    if (!s_heart_overlay.active) return;

    if (overlay_alpha(now_us, s_heart_overlay.start_us,
                      s_heart_overlay.duration_ms) <= 0.0f) {
        s_heart_overlay.active = false;
        return;
    }

    int16_t eye_right = (int16_t)(right_cx + HW_I);
    int16_t eye_top = (int16_t)(eye_cy - MAX_HH_F);
    int64_t elapsed_us = now_us - s_heart_overlay.start_us;
    float phase = (float)(elapsed_us % 900000LL) / 900000.0f;
    float pulse = sinf(phase * 2.0f * NB_PI_F);
    int16_t size = (int16_t)(17 + (pulse > 0.35f ? 1 : 0));
    int16_t cx = (int16_t)(eye_right + 12);
    int16_t cy = (int16_t)(eye_top - 10 + (pulse > 0.0f ? -1 : 0));
    uint32_t color = 0xFF6F86u;

    if (cx > 294) cx = 294;
    if (cy < 54) cy = 54;
    draw_solid_heart(spr, cx, cy, size, 0.2617994f, color);
}


/* Helpers de coordenada para draw_anger_mark (via draw_bubble_cubic). */
static inline int16_t bubble_point_x(float ax, float px, float scale)
{
    return (int16_t)(ax + px * scale + (px >= 0.0f ? 0.5f : -0.5f));
}
static inline int16_t bubble_point_y(float ay, float py, float scale)
{
    return (int16_t)(ay + py * scale + (py >= 0.0f ? 0.5f : -0.5f));
}

static void draw_bubble_cubic(LGFX_Sprite *spr,
                              float anchor_x, float anchor_y, float scale,
                              float x0, float y0,
                              float x1, float y1,
                              float x2, float y2,
                              float x3, float y3,
                              uint32_t color)
{
    int16_t prev_x = bubble_point_x(anchor_x, x0, scale);
    int16_t prev_y = bubble_point_y(anchor_y, y0, scale);

    for (int i = 1; i <= 14; ++i) {
        float t = (float)i / 14.0f;
        float u = 1.0f - t;
        float x = u * u * u * x0
                + 3.0f * u * u * t * x1
                + 3.0f * u * t * t * x2
                + t * t * t * x3;
        float y = u * u * u * y0
                + 3.0f * u * u * t * y1
                + 3.0f * u * t * t * y2
                + t * t * t * y3;
        int16_t next_x = bubble_point_x(anchor_x, x, scale);
        int16_t next_y = bubble_point_y(anchor_y, y, scale);
        draw_overlay_line_thick(spr, prev_x, prev_y, next_x, next_y, color, 1);
        prev_x = next_x;
        prev_y = next_y;
    }
}

static void draw_bubble_cubic_thick(LGFX_Sprite *spr,
                                    float anchor_x, float anchor_y, float scale,
                                    float x0, float y0,
                                    float x1, float y1,
                                    float x2, float y2,
                                    float x3, float y3,
                                    uint32_t color)
{
    draw_bubble_cubic(spr, anchor_x, anchor_y, scale,
                      x0, y0, x1, y1, x2, y2, x3, y3, color);
    draw_bubble_cubic(spr, anchor_x - 2.0f, anchor_y, scale,
                      x0, y0, x1, y1, x2, y2, x3, y3, color);
    draw_bubble_cubic(spr, anchor_x + 2.0f, anchor_y, scale,
                      x0, y0, x1, y1, x2, y2, x3, y3, color);
    draw_bubble_cubic(spr, anchor_x, anchor_y - 2.0f, scale,
                      x0, y0, x1, y1, x2, y2, x3, y3, color);
    draw_bubble_cubic(spr, anchor_x, anchor_y + 2.0f, scale,
                      x0, y0, x1, y1, x2, y2, x3, y3, color);
    draw_bubble_cubic(spr, anchor_x - 1.5f, anchor_y - 1.5f, scale,
                      x0, y0, x1, y1, x2, y2, x3, y3, color);
    draw_bubble_cubic(spr, anchor_x + 1.5f, anchor_y + 1.5f, scale,
                      x0, y0, x1, y1, x2, y2, x3, y3, color);
}

static void draw_anger_mark(LGFX_Sprite *spr)
{
    const float anchor_x = 242.0f;
    const float anchor_y = 78.0f;
    const float scale    = 1.0f;
    const uint32_t red   = 0xFF5C66u;
    const uint32_t soft  = blend_with_black(red, 0.55f);

    /* Marca de raiva estilo manga: quatro traços curvos acima do olho direito. */
    draw_bubble_cubic_thick(spr, anchor_x, anchor_y, scale,
                            -22.0f, -17.0f, -12.0f, -14.0f, -11.0f, -2.0f, -20.0f, 4.0f,
                            red);
    draw_bubble_cubic_thick(spr, anchor_x, anchor_y, scale,
                            7.0f, -17.0f, 0.0f, -10.0f, 2.0f, 0.0f, 14.0f, 2.0f,
                            red);
    draw_bubble_cubic_thick(spr, anchor_x, anchor_y, scale,
                            -4.0f, 15.0f, -1.0f, 7.0f, 5.0f, 7.0f, 11.0f, 15.0f,
                            red);
    draw_bubble_cubic(spr, anchor_x + 1.0f, anchor_y + 1.0f, scale,
                      -22.0f, -17.0f, -12.0f, -14.0f, -11.0f, -2.0f, -20.0f, 4.0f,
                      soft);
    draw_bubble_cubic(spr, anchor_x + 1.0f, anchor_y + 1.0f, scale,
                      7.0f, -17.0f, 0.0f, -10.0f, 2.0f, 0.0f, 14.0f, 2.0f,
                      soft);
    draw_bubble_cubic(spr, anchor_x + 1.0f, anchor_y + 1.0f, scale,
                      -4.0f, 15.0f, -1.0f, 7.0f, 5.0f, 7.0f, 11.0f, 15.0f,
                      soft);
}

static void draw_speaking_mouth(LGFX_Sprite *spr,
                                int64_t now_us,
                                uint16_t color,
                                float left_eye_bottom,
                                float right_eye_bottom)
{
    if (!s_speaking_mouth_enabled) return;

    int64_t elapsed_us = now_us - s_speaking_mouth_start_us;
    if (elapsed_us < 0) elapsed_us = 0;

    float phase = (float)(elapsed_us % MOUTH_PERIOD_US) / (float)MOUTH_PERIOD_US;
    float wave  = 0.5f + 0.5f * sinf(phase * 2.0f * NB_PI_F);
    float bite  = 0.5f + 0.5f * sinf((phase * 3.0f + 0.18f) * 2.0f * NB_PI_F);
    float open  = (wave * 0.72f) + (bite * 0.28f);

    int16_t mouth_w = (int16_t)((float)MOUTH_MIN_W
                     + ((float)(MOUTH_MAX_W - MOUTH_MIN_W) * (0.35f + open * 0.65f))
                     + 0.5f);
    int16_t mouth_h = (int16_t)((float)MOUTH_MIN_H
                     + ((float)(MOUTH_MAX_H - MOUTH_MIN_H) * open)
                     + 0.5f);
    float eye_bottom = (left_eye_bottom > right_eye_bottom) ? left_eye_bottom : right_eye_bottom;
    int16_t mouth_x = MOUTH_CX - (mouth_w / 2);
    int16_t mouth_y = MOUTH_CY - (mouth_h / 2);
    int16_t safe_y = (int16_t)(eye_bottom + (float)MOUTH_EYE_GAP_PX + 0.5f);
    if (mouth_y < safe_y) mouth_y = safe_y;
    if ((mouth_y + mouth_h) > MOUTH_MAX_BOTTOM_Y) {
        mouth_y = (int16_t)(MOUTH_MAX_BOTTOM_Y - mouth_h);
    }
    int16_t radius  = mouth_h / 2;
    if (radius < 2) radius = 2;

    spr->fillRoundRect(mouth_x, mouth_y, mouth_w, mouth_h, radius, color);
}

static void apply_speaking_pose(nb_face_state_t *face)
{
    if (!face) return;

    uint32_t color = face->color;
    *face = NB_EXPRESSIONS[NB_EXPR_NEUTRAL];
    face->color = color;
    face->open_l = SPEAKING_EYE_OPEN;
    face->open_r = SPEAKING_EYE_OPEN;
    face->y_l = SPEAKING_EYE_Y_NORM;
    face->y_r = SPEAKING_EYE_Y_NORM;
    face->squint_l = 0.0f;
    face->squint_r = 0.0f;
}

/* ── Blink ───────────────────────────────────────────────────────────────── */

static int64_t poisson_blink_delay_us(void)
{
    uint32_t rnd = esp_random();
    float u      = (float)(rnd | 1u) / 4294967296.0f;
    float delay  = -BLINK_MEAN_MS * std::log(u);
    if (delay < BLINK_MIN_MS) delay = BLINK_MIN_MS;
    if (delay > 9000.0f)      delay = 9000.0f;
    return (int64_t)(delay * 1000.0f);
}

/*
 * Dispara um blink bilateral, opcionalmente assimétrico.
 * Chame quando now_us >= s_next_blink_us e ambos os olhos em IDLE.
 */
static void blink_trigger_bilateral(int64_t now_us)
{
    s_blink[0].state = BLINK_CLOSING;
    s_blink[1].state = BLINK_CLOSING;

    uint32_t rnd = esp_random();
    if ((rnd & 0xFFu) < BLINK_ASYM_THRESH) {
        /*
         * Assimetria: qual olho atrasa é sorteado a cada blink (bit 16 do rng).
         * Evita o padrão de direito sempre atrasado, tornando o efeito mais natural.
         */
        int64_t delay    = BLINK_ASYM_MIN_US
                         + (int64_t)(((rnd >> 8) & 0x7Fu) * (BLINK_ASYM_RANGE / 128LL));
        int     late_eye = (int)((rnd >> 16) & 1u);   /* 0 = esq atrasa, 1 = dir atrasa */
        s_blink[late_eye].t0     = now_us + delay;
        s_blink[1 - late_eye].t0 = now_us;
    } else {
        s_blink[0].t0 = now_us;
        s_blink[1].t0 = now_us;
    }
}

/*
 * is_left: true para o olho esquerdo.
 * Quando o olho ESQUERDO termina de abrir (→ IDLE), agenda o próximo blink
 * com distribuição de Poisson — exato mesmo comportamento do código original.
 */
static void blink_update_eye(nb_blink_eye_t *eye, int64_t now_us, bool is_left)
{
    int64_t dt = now_us - eye->t0;

    switch (eye->state) {
        case BLINK_IDLE:
            break;

        case BLINK_CLOSING:
            if (dt < 0) break;   /* t0 no futuro (assimetria) — aguarda */
            {
                /* Smoothstep: sigmoidal — fechamento natural, acelera no meio */
                float t = clamp01((float)dt / (float)BLINK_CLOSE_US);
                eye->phase = t * t * (3.0f - 2.0f * t);
            }
            if (dt >= BLINK_CLOSE_US) {
                eye->phase = 1.0f;
                eye->state = BLINK_CLOSED;
                eye->t0    = now_us;
            }
            break;

        case BLINK_CLOSED:
            if (dt >= BLINK_HOLD_US) {
                eye->state = BLINK_OPENING;
                eye->t0    = now_us;
            }
            break;

        case BLINK_OPENING:
            {
                /* Ease-out quadrático: (1-t)^2 — abre rápido, desacelera ao final */
                float t = clamp01((float)dt / (float)BLINK_OPEN_US);
                eye->phase = (1.0f - t) * (1.0f - t);
            }
            if (dt >= BLINK_OPEN_US) {
                eye->phase = 0.0f;
                eye->state = BLINK_IDLE;
                if (is_left) {
                    s_next_blink_us = now_us + poisson_blink_delay_us();
                    /* ~12% chance de duplo blink: segundo blink 180–380ms depois */
                    if (!s_double_blink_pending &&
                        (esp_random() & 0xFFu) < DOUBLE_BLINK_THRESH) {
                        s_double_blink_pending = true;
                        s_double_blink_us = now_us + DOUBLE_BLINK_MIN_US
                                          + (int64_t)((esp_random() & 0xFFu)
                                            * (DOUBLE_BLINK_RNG_US / 256LL));
                    }
                }
            }
            break;
    }
}

static void blink_update(int64_t now_us)
{
    if (!s_blink_enabled) {
        if (s_blink[0].state != BLINK_IDLE || s_blink[1].state != BLINK_IDLE) {
            s_blink[0] = { BLINK_IDLE, 0.0f, now_us };
            s_blink[1] = { BLINK_IDLE, 0.0f, now_us };
            s_next_blink_us = now_us + poisson_blink_delay_us();
        }
        s_double_blink_pending = false;
        s_blink_prev_enabled   = false;
        return;
    }

    /* Ao reabilitar: agenda delay fresco para não disparar imediatamente
     * (s_next_blink_us está no passado após um período com blink desativado). */
    if (!s_blink_prev_enabled) {
        s_next_blink_us      = now_us + poisson_blink_delay_us();
        s_blink_prev_enabled = true;
    }

    /* Disparo de novo blink apenas quando ambos os olhos estiverem em IDLE */
    if (s_blink[0].state == BLINK_IDLE && s_blink[1].state == BLINK_IDLE) {
        if (now_us >= s_next_blink_us) {
            blink_trigger_bilateral(now_us);
        }
    }

    blink_update_eye(&s_blink[0], now_us, true);
    blink_update_eye(&s_blink[1], now_us, false);

    /* Duplo blink: dispara quando ambos os olhos voltaram a IDLE */
    if (s_double_blink_pending &&
        s_blink[0].state == BLINK_IDLE && s_blink[1].state == BLINK_IDLE &&
        now_us >= s_double_blink_us) {
        blink_trigger_bilateral(now_us);
        s_double_blink_pending = false;
    }
}

/* ── Renderer do olho EMO ────────────────────────────────────────────────── */

/*
 * Desenha um olho no estilo EMO: quadrilátero paramétrico com curvatura,
 * arredondamento de cantos e squint da pálpebra superior.
 *
 * @param spr        Canvas LGFX_Sprite de destino.
 * @param base_cx    Centro X base do olho (antes do x_off).
 * @param cy_base    Centro Y pré-computado em pixels (inclui EYE_CY_BASE, open e y_off).
 * @param open       Abertura vertical [0..1].
 * @param tl/tr      Corner offsets superiores [0..1] (+ = fecha).
 * @param bl/br      Corner offsets inferiores [0..1] (+ = fecha).
 * @param squint     Descida da pálpebra superior [0..1].
 * @param rt_top     Arredondamento cantos superiores [0..1].
 * @param rb_bot     Arredondamento cantos inferiores [0..1].
 * @param cv_top     Curvatura borda superior [-1..1] (+ = convexa).
 * @param cv_bot     Curvatura borda inferior [-1..1] (+ = convexa).
 * @param blink_ph   Fase do blink [0..1] (1 = totalmente fechado).
 * @param color      Cor do olho.
 */
static void draw_emo_eye(LGFX_Sprite *spr,
                         int16_t base_cx, float cy_base,
                         float open,
                         float tl, float tr, float bl, float br,
                         float squint,
                         float rt_top, float rb_bot,
                         float cv_top, float cv_bot,
                         float blink_ph,
                         uint32_t color)
{
    float eff_open = open * (1.0f - blink_ph);
    if (eff_open < 0.0f) eff_open = 0.0f;

    float cy_f  = cy_base;
    int16_t cx  = base_cx;
    int16_t cy  = (int16_t)(cy_f + 0.5f);

    /* Fallback: eff_open numericamente zero (fase de barra é tratada no render_layer_cb) */
    if (eff_open < 0.03f) {
        spr->drawFastHLine(cx - HW_I, cy, HW_I * 2, color);
        return;
    }

    float hh = eff_open * MAX_HH_F;

    /*
     * Coordenadas Y dos quatro cantos do quadrilátero.
     *
     * Topo:   tl/tr > 0 → canto desce em direção à linha central
     *         y_tl = cy_f - hh*(1-tl)   → tl=0: cy-hh;  tl=1: cy (fecha)
     *
     * Fundo:  bl/br > 0 → canto sobe em direção à linha central
     *         y_bl = cy_f + hh*(1-bl)   → bl=0: cy+hh;  bl=1: cy (fecha)
     */
    float y_tl = cy_f - hh * (1.0f - tl);
    float y_tr = cy_f - hh * (1.0f - tr);
    float y_bl = cy_f + hh * (1.0f - bl);
    float y_br = cy_f + hh * (1.0f - br);

    /* Squint: pálpebra superior desce */
    float squint_px = squint * hh * 0.65f;

    /* Raios de arredondamento em pixels — limitado por hh para não ultrapassar o olho */
    float r_top = rt_top * HW_F * 0.68f;
    float r_bot = rb_bot * HW_F * 0.68f;
    if (r_top > hh) r_top = hh;
    if (r_bot > hh) r_bot = hh;

    /* Varredura coluna-a-coluna */
    for (int16_t x = cx - HW_I; x <= cx + HW_I; x++) {
        /* t ∈ [0..1] ao longo da largura do olho */
        float t = (float)(x - (cx - HW_I)) / (float)(HW_I * 2);

        /* Borda superior e inferior: interpolação linear entre cantos */
        float top_y = y_tl + t * (y_tr - y_tl);
        float bot_y = y_bl + t * (y_br - y_bl);

        /* Curvatura: parábola com pico no centro (t = 0.5) */
        float para = 4.0f * t * (1.0f - t);   /* [0..1], 1 no centro */
        top_y -= cv_top * MAX_CURVE_PX * para;
        bot_y += cv_bot * MAX_CURVE_PX * para;

        /* Arredondamento dos cantos: arco CIRCULAR — mesmo perfil de fillRoundRect */
        float dist_l = (float)(x - (cx - HW_I));
        float dist_r = (float)((cx + HW_I) - x);
        float edge   = (dist_l < dist_r) ? dist_l : dist_r;

        if (r_top > 0.0f && edge < r_top) {
            float t = 1.0f - edge / r_top;          /* 1 no canto, 0 na transição */
            top_y += r_top * (1.0f - std::sqrt(1.0f - t * t));
        }
        if (r_bot > 0.0f && edge < r_bot) {
            float t = 1.0f - edge / r_bot;
            bot_y -= r_bot * (1.0f - std::sqrt(1.0f - t * t));
        }

        /* Pálpebra superior: linha de corte do squint */
        float draw_top = top_y + squint_px;

        if (draw_top >= bot_y - 0.5f) continue;   /* coluna totalmente fechada */

        /*
         * Anti-aliasing sub-pixel:
         *
         *   draw_top = 10.3  →  pixel 10 com alpha 0.7 (parcial)
         *                       pixels 11..bot_full com cor cheia
         *
         *   bot_y    = 25.8  →  pixels top_full..25 com cor cheia
         *                       pixel 26 com alpha 0.8 (parcial)
         *
         * blend_with_black() compõe o pixel de borda contra TFT_BLACK,
         * que é garantido pelo canvas limpo do render_service.
         */
        int16_t top_full = (int16_t)ceilf(draw_top);
        int16_t bot_full = (int16_t)floorf(bot_y);

        float alpha_top = (float)top_full - draw_top;   /* [0..1] */
        float alpha_bot = bot_y - (float)bot_full;       /* [0..1] */

        /* Pixel superior parcial */
        if (alpha_top > 0.04f) {
            spr->drawPixel(x, top_full - 1, blend_with_black(color, alpha_top));
        }

        /* Corpo cheio */
        if (bot_full >= top_full) {
            spr->drawFastVLine(x, top_full, bot_full - top_full + 1, color);
        }

        /* Pixel inferior parcial */
        if (alpha_bot > 0.04f) {
            spr->drawPixel(x, bot_full + 1, blend_with_black(color, alpha_bot));
        }
    }
}

/* ── Callback de layer ───────────────────────────────────────────────────── */

static void render_layer_cb(nb_display_sprite_t canvas_handle, void * /*ctx*/)
{
    LGFX_Sprite *spr  = static_cast<LGFX_Sprite *>(canvas_handle);
    int64_t now_us    = esp_timer_get_time();

    /*
     * ── Resolução de target (prioridade: base update > play queue > sem mudança) ──
     *
     * Base update (expression_service_set): maior prioridade — cancela play ativo.
     * Play queue (expression_play):         só inicia quando PLAY_STATE_IDLE.
     */
    bool target_changed = false;

    if (s_wake_seq_pending) {
        if (xSemaphoreTake(s_set_mutex, 0) == pdTRUE) {
            if (s_wake_seq_pending) {
                s_wake_seq_pending    = false;
                s_wake_seq_active     = true;
                s_wake_seq_elapsed_ms = 0.0f;
                s_play_state          = PLAY_STATE_IDLE;
                s_play_count          = 0;
                s_base_expr           = NB_EXPR_NEUTRAL;
                s_active_expr         = NB_EXPR_NEUTRAL;
                s_target              = NB_EXPRESSIONS[NB_EXPR_NEUTRAL];
                sync_led_mood(NB_EXPR_NEUTRAL);
            }
            xSemaphoreGive(s_set_mutex);
        }
    }

    if (s_new_target_pending) {
        if (xSemaphoreTake(s_set_mutex, 0) == pdTRUE) {
            if (s_new_target_pending) {
                s_wake_seq_active   = false;
                s_base_expr          = s_pending_base_expr;
                s_from               = s_current;
                s_target             = s_pending_target;
                s_trans_total_ms     = s_pending_trans_ms;
                s_trans_elapsed_ms   = 0.0f;
                s_new_target_pending = false;
                s_play_state         = PLAY_STATE_IDLE;   /* cancela play ativo */
                s_active_expr        = s_base_expr;
                sync_led_mood(s_active_expr);
                target_changed       = true;
            }
            xSemaphoreGive(s_set_mutex);
        }
    }

    if (!target_changed) {
        switch (s_play_state) {

            case PLAY_STATE_IDLE: {
                play_item_t item = {};
                bool got = false;
                if (xSemaphoreTake(s_set_mutex, 0) == pdTRUE) {
                    if (s_play_count > 0 && !s_new_target_pending) {
                        item          = s_play_queue[s_play_head];
                        s_play_head   = (s_play_head + 1) % PLAY_QUEUE_CAP;
                        s_play_count--;
                        got = true;
                    }
                    xSemaphoreGive(s_set_mutex);
                }
                if (got) {
                    s_from             = s_current;
                    s_target           = NB_EXPRESSIONS[item.expr];
                    s_trans_total_ms   = item.trans_ms;
                    s_trans_elapsed_ms = 0.0f;
                    s_play_state       = PLAY_STATE_HOLD;
                    s_play_elapsed_ms  = 0.0f;
                    s_play_dur_ms      = item.duration_ms;
                    s_play_tr_ms       = item.trans_ms;
                    s_active_expr      = item.expr;
                    sync_led_mood(s_active_expr);
                }
                break;
            }

            case PLAY_STATE_HOLD:
                s_play_elapsed_ms += FRAME_MS;
                if (s_play_elapsed_ms >= s_play_dur_ms) {
                    /* Inicia retorno à expressão base */
                    s_from             = s_current;
                    s_target           = NB_EXPRESSIONS[s_base_expr];
                    s_trans_total_ms   = s_play_tr_ms;
                    s_trans_elapsed_ms = 0.0f;
                    s_play_state       = PLAY_STATE_OUT;
                    s_play_ret_ms      = 0.0f;
                    s_active_expr      = s_base_expr;
                    sync_led_mood(s_active_expr);
                }
                break;

            case PLAY_STATE_OUT:
                s_play_ret_ms += FRAME_MS;
                if (s_play_ret_ms >= s_play_tr_ms) {
                    s_play_state = PLAY_STATE_IDLE;   /* verifica fila no próximo frame */
                }
                break;
        }
    }

    if (s_wake_seq_active) {
        s_wake_seq_elapsed_ms += FRAME_MS;
        float pos = s_wake_seq_elapsed_ms / WAKE_SEQ_FRAME_MS;
        int idx = (int)floorf(pos);
        if (idx >= WAKE_SEQ_COUNT - 1) {
            s_wake_seq_active = false;
            s_current = NB_EXPRESSIONS[NB_EXPR_NEUTRAL];
            s_from = s_current;
            s_target = s_current;
            s_trans_total_ms = 0.0f;
            s_trans_elapsed_ms = 0.0f;
        } else {
            float t = pos - (float)idx;
            nb_face_state_lerp(&k_wake_sequence[idx], &k_wake_sequence[idx + 1], t, &s_current);
        }
    } else if (s_trans_elapsed_ms < s_trans_total_ms && s_trans_total_ms > 0.0f) {
        /* Interpolação de estado */
        s_trans_elapsed_ms += FRAME_MS;
        float t = s_trans_elapsed_ms / s_trans_total_ms;
        if (t > 1.0f) t = 1.0f;
        nb_face_state_lerp(&s_from, &s_target, t, &s_current);
    } else {
        s_current = s_target;
    }

    bool wake_anim = s_wake_seq_active;
    bool speaking_anim = s_speaking_mouth_enabled;

    /* Blink (atualiza fase de cada olho) */
    blink_update(now_us);
    if (wake_anim) {
        s_blink[0].phase = 0.0f;
        s_blink[1].phase = 0.0f;
    }

    nb_face_state_t face = s_current;
    bool sleep_anim = s_sleep_anim_enabled;
    if (s_breath_enabled && !wake_anim) {
        float t_ms = (float)(now_us % (int64_t)(BREATH_PERIOD_MS * 1000.0f)) / 1000.0f;
        float breath = sinf((t_ms / BREATH_PERIOD_MS) * 2.0f * NB_PI_F);
        float scale = 1.0f + breath * BREATH_AMP;
        face.open_l *= scale;
        face.open_r *= scale;
        if (face.open_l < 0.05f) face.open_l = 0.05f;
        if (face.open_r < 0.05f) face.open_r = 0.05f;
        if (face.open_l > 1.5f) face.open_l = 1.5f;
        if (face.open_r > 1.5f) face.open_r = 1.5f;
    }

    float sleep_bob_norm = 0.0f;
    if (sleep_anim) {
        int64_t elapsed_us = now_us - s_sleep_anim_start_us;
        if (elapsed_us < 0) elapsed_us = 0;
        apply_sleep_visual_stage(&face, (float)elapsed_us / 1000.0f, &sleep_bob_norm);
    } else if (speaking_anim && !wake_anim) {
        apply_speaking_pose(&face);
    }

    /* Centros X dos olhos com x_off (convergência) e gaze_x (translation) aplicados */
    float   gx        = (sleep_anim || wake_anim || speaking_anim) ? 0.0f : s_gaze_x;
    float   gy        = (sleep_anim || wake_anim || speaking_anim) ? 0.0f
                                   : damp_vertical_for_lateral_gaze(gx, clamp_abs(s_gaze_y, GAZE_Y_MAX));
    int16_t gaze_shift = (int16_t)(gx * GAZE_X_TRAVEL_PX + (gx >= 0.0f ? 0.5f : -0.5f));
    int16_t left_cx   = BASE_L_CX
                      + (int16_t)(face.x_off * X_OFF_TRAVEL + 0.5f)
                      + gaze_shift;
    int16_t right_cx  = BASE_R_CX
                      - (int16_t)(face.x_off * X_OFF_TRAVEL + 0.5f)
                      + gaze_shift;

    /* Idle overlay assimétrico (head_tilt, curious_tilt) — aditivo, contornável
     * pelos clamps abaixo. Em sleep_anim ignoramos para não interferir. */
    float dy_l_ovl    = (sleep_anim || wake_anim || speaking_anim) ? 0.0f : s_idle_dy_l;
    float dy_r_ovl    = (sleep_anim || wake_anim || speaking_anim) ? 0.0f : s_idle_dy_r;
    float dopen_l_ovl = (sleep_anim || wake_anim || speaking_anim) ? 0.0f : s_idle_dopen_l;
    float dopen_r_ovl = (sleep_anim || wake_anim || speaking_anim) ? 0.0f : s_idle_dopen_r;

    /* Offsets Y combinados com gaze_y e overlay.
     * Em sleep_anim, face.y_l/y_r já contêm o valor animado e gy=0. */
    float y_l = clamp_abs(face.y_l + gy + dy_l_ovl, GAZE_Y_MAX);
    float y_r = clamp_abs(face.y_r + gy + dy_r_ovl, GAZE_Y_MAX);
    if (sleep_anim) {
        y_l += sleep_bob_norm;
        y_r += sleep_bob_norm;
    }

    /* Overlay de abertura (curious_tilt: um olho mais aberto). Aplicado antes
     * da perspectiva de gaze para que ambos efeitos se combinem naturalmente.
     * Clampado em [0, 1.5] — open_r pode passar de 1 (ex: SURPRISED 1.10). */
    if (dopen_l_ovl != 0.0f) {
        float v = face.open_l + dopen_l_ovl;
        face.open_l = v < 0.0f ? 0.0f : (v > 1.5f ? 1.5f : v);
    }
    if (dopen_r_ovl != 0.0f) {
        float v = face.open_r + dopen_r_ovl;
        face.open_r = v < 0.0f ? 0.0f : (v > 1.5f ? 1.5f : v);
    }
    /* Centro médio dos olhos com base inferior alinhada. */
    float open_avg = (face.open_l + face.open_r) * 0.5f;
    float eye_cy_f = (float)EYE_CY_BASE + (1.0f - open_avg) * MAX_HH_F
                   + ((y_l + y_r) * 0.5f * Y_TRAVEL_PX);
    int16_t eye_cy = (int16_t)(eye_cy_f + (eye_cy_f >= 0.0f ? 0.5f : -0.5f));
    ui_overlay_set_eye_frame(left_cx, right_cx, eye_cy);

    /* Perspectiva lateral: olho do lado do gaze estreita levemente. */
    if (!sleep_anim && fabsf(gx) > 0.04f) {
        float persp = clamp01((fabsf(gx) - 0.04f) / (GAZE_X_MAX - 0.04f));
        if (gx < 0.0f) {
            face.open_l   *= (1.0f - persp * GAZE_PERSP_OPEN_FACTOR);
            face.squint_l  = clamp01(face.squint_l + persp * GAZE_PERSP_SQUINT_ADD);
        } else {
            face.open_r   *= (1.0f - persp * GAZE_PERSP_OPEN_FACTOR);
            face.squint_r  = clamp01(face.squint_r + persp * GAZE_PERSP_SQUINT_ADD);
        }
    }

    /*
     * Perspectiva vertical: torna o gaze vertical legível pela forma do olho.
     *   Olhar para cima (gy < 0): squint aumenta — pálpebra superior desce,
     *     lê como "olhos subindo atrás da pálpebra".
     *   Olhar para baixo (gy > 0): cv_top sobe e open reduz levemente —
     *     topo do olho achata, lê como "olho pesando para baixo".
     * Threshold 0.08 evita ruído do micro-drift.
     */
    if (!sleep_anim && fabsf(gy) > 0.08f) {
        float vpersp = clamp01((fabsf(gy) - 0.08f) / (GAZE_Y_MAX - 0.08f));
        if (gy < 0.0f) {
            /* Olhando para cima: squint leve em ambos os olhos */
            float sq_add = vpersp * 0.18f;
            face.squint_l = clamp01(face.squint_l + sq_add);
            face.squint_r = clamp01(face.squint_r + sq_add);
        } else {
            /* Olhando para baixo: topo do olho achata (cv_top cai) e open reduz */
            float cv_delta  = vpersp * 0.30f;
            float open_mult = 1.0f - vpersp * 0.12f;
            face.cv_top  -= cv_delta;
            face.open_l  *= open_mult;
            face.open_r  *= open_mult;
        }
    }

    /* Canvas já limpo em TFT_BLACK pelo render_service */

    /* Centro Y de cada olho com base inferior alinhada (pré-computado aqui,
     * passado explicitamente para draw_emo_eye e reutilizado no sprite path). */
    float cy_l_f = (float)EYE_CY_BASE + (1.0f - face.open_l) * MAX_HH_F + y_l * Y_TRAVEL_PX;
    float cy_r_f = (float)EYE_CY_BASE + (1.0f - face.open_r) * MAX_HH_F + y_r * Y_TRAVEL_PX;

    /* Rotação idle (POSE_TILT). Float 32-bit é atômico em ESP32-S3. */
    float rot_l = s_idle_rot_l;
    bool  is_neutral = (s_active_expr == NB_EXPR_NEUTRAL);
    /* Rotação: usa rot_l como ângulo único para ambos os olhos (sempre igual).
     * Sprite combinado 320×96 garante que os dois giram ao redor do centro
     * da face (x=160), não ao redor do centro individual de cada olho. */
    bool  use_rot = !sleep_anim && !wake_anim && !speaking_anim && is_neutral && s_face_spr_ready
                 && (rot_l > 0.5f || rot_l < -0.5f);

    /*
     * Blink bar EMO: quando qualquer olho entra na fase de barra, os dois olhos
     * são substituídos por uma barra única que vai da borda externa do olho
     * esquerdo à borda externa do olho direito (+ padding). Mais larga que
     * cada olho individual — visual unificado característico do estilo EMO.
     */
    if (s_blink[0].phase > BLINK_BAR_PH_THRESH ||
        s_blink[1].phase > BLINK_BAR_PH_THRESH) {

        int16_t bx       = left_cx  - HW_I - BLINK_BAR_EXTRA_HW;
        int16_t bw       = (right_cx + HW_I + BLINK_BAR_EXTRA_HW) - bx;

        /* Blink bar na base inferior: usa open pós-perspectiva (valores finais). */
        float open_avg_final = (face.open_l + face.open_r) * 0.5f;
        float bar_y_f = (float)EYE_CY_BASE + (1.0f - open_avg_final) * MAX_HH_F
                      + ((y_l + y_r) * 0.5f * Y_TRAVEL_PX);
        int16_t bar_y = (int16_t)(bar_y_f + 0.5f);

        spr->drawFastHLine(bx, bar_y - 1, bw, face.color);
        spr->drawFastHLine(bx, bar_y,     bw, face.color);
        spr->drawFastHLine(bx, bar_y + 1, bw, face.color);

    } else if (use_rot) {

        /* Rotação combinada: ambos os olhos num sprite único 320×96.
         * Desenhados nas posições de tela (x = left_cx / right_cx).
         * Y no sprite = SPR_CYF + (1-open)*MAX_HH_F (bottom-aligned, centrado no sprite).
         * pushRotateZoom ao redor do centro da face (SPR_FCX=160, SPR_CYF=48)
         * → os dois olhos arcos ao redor de x=160, efeito de head tilt real. */
        s_face_spr.fillSprite(TFT_BLACK);

        float spr_cy_l = SPR_CYF + (1.0f - face.open_l) * MAX_HH_F;
        float spr_cy_r = SPR_CYF + (1.0f - face.open_r) * MAX_HH_F;

        draw_emo_eye(&s_face_spr,
                     left_cx, spr_cy_l,
                     face.open_l,
                     face.tl_l, face.tr_l,
                     face.bl_l, face.br_l,
                     face.squint_l,
                     face.rt_top, face.rb_bot,
                     face.cv_top, face.cv_bot,
                     s_blink[0].phase,
                     face.color);

        draw_emo_eye(&s_face_spr,
                     right_cx, spr_cy_r,
                     face.open_r,
                     face.tl_r, face.tr_r,
                     face.bl_r, face.br_r,
                     face.squint_r,
                     face.rt_top, face.rb_bot,
                     face.cv_top, face.cv_bot,
                     s_blink[1].phase,
                     face.color);

        /* Push: sprite center (160, 48) mapeia para (160, push_y) na tela.
         * push_y = EYE_CY_BASE + avg_y*Y_TRAVEL_PX (base da face sem open offset,
         * já que open offset foi absorvido em spr_cy_l/r). */
        float push_y = (float)EYE_CY_BASE + ((y_l + y_r) * 0.5f * Y_TRAVEL_PX);
        s_face_spr.pushRotateZoom(spr, SPR_FCX, push_y, rot_l, 1.0f, 1.0f, TFT_BLACK);

    } else {

        /* Path normal sem rotação */
        draw_emo_eye(spr,
                     left_cx, cy_l_f,
                     face.open_l,
                     face.tl_l, face.tr_l,
                     face.bl_l, face.br_l,
                     face.squint_l,
                     face.rt_top, face.rb_bot,
                     face.cv_top, face.cv_bot,
                     s_blink[0].phase,
                     face.color);

        draw_emo_eye(spr,
                     right_cx, cy_r_f,
                     face.open_r,
                     face.tl_r, face.tr_r,
                     face.bl_r, face.br_r,
                     face.squint_r,
                     face.rt_top, face.rb_bot,
                     face.cv_top, face.cv_bot,
                     s_blink[1].phase,
                     face.color);
    }

    int16_t cy_l_i = (int16_t)(cy_l_f + (cy_l_f >= 0.0f ? 0.5f : -0.5f));
    int16_t cy_r_i = (int16_t)(cy_r_f + (cy_r_f >= 0.0f ? 0.5f : -0.5f));
    if (!sleep_anim) {
        float eye_bottom_l = cy_l_f + face.open_l * MAX_HH_F;
        float eye_bottom_r = cy_r_f + face.open_r * MAX_HH_F;
        draw_speaking_mouth(spr, now_us, face.color, eye_bottom_l, eye_bottom_r);
    }
    draw_blush_overlay(spr, now_us, left_cx, right_cx, cy_l_i, cy_r_i);
    draw_heart_overlay(spr, now_us, right_cx, eye_cy);
    if (!sleep_anim && (s_active_expr == NB_EXPR_SUSPICIOUS ||
                        s_active_expr == NB_EXPR_ALARMED ||
                        s_active_expr == NB_EXPR_ANGRY)) {
        draw_anger_mark(spr);
    }

    /* Declara região suja. Rect conservador cobre todos os pixels possíveis
     * dos olhos (gaze shift, x_off, blink bar). Ser fixo garante que pixels
     * do frame anterior — sempre dentro do mesmo envelope — sejam cobertos
     * pelo canvas limpo e efetivamente apagados no display. */
    render_service_mark_dirty(FACE_DIRTY_X0, FACE_DIRTY_Y0,
                              FACE_DIRTY_X1 - FACE_DIRTY_X0,
                              FACE_DIRTY_Y1 - FACE_DIRTY_Y0);
}

/* ── API pública (extern "C") ────────────────────────────────────────────── */

extern "C" {

esp_err_t expression_service_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "expression_service_init chamado mais de uma vez");
        return ESP_ERR_INVALID_STATE;
    }

    s_set_mutex = xSemaphoreCreateMutex();
    if (!s_set_mutex) {
        ESP_LOGE(TAG, "falha ao criar mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Sprite de face combinado para rotação (POSE_TILT). Alocado em PSRAM.
     * 320×96px — dois olhos desenhados nas posições de tela, rotacionados
     * como unidade ao redor do centro da face (x=160). */
    s_face_spr.setPsram(true);
    if (!s_face_spr.createSprite(SPR_W, SPR_H)) {
        ESP_LOGW(TAG, "face sprite nao alocado — rotacao desabilitada");
        s_face_spr.deleteSprite();
        s_face_spr_ready = false;
    } else {
        s_face_spr.setColorDepth(16);
        s_face_spr_ready = true;
        ESP_LOGI(TAG, "face sprite %dx%d criado em PSRAM", SPR_W, SPR_H);
    }

    s_current          = NB_EXPRESSIONS[NB_EXPR_NEUTRAL];
    s_target           = NB_EXPRESSIONS[NB_EXPR_NEUTRAL];
    s_from             = NB_EXPRESSIONS[NB_EXPR_NEUTRAL];
    s_active_expr      = NB_EXPR_NEUTRAL;
    sync_led_mood(s_active_expr);
    s_trans_total_ms   = 0.0f;
    s_trans_elapsed_ms = 0.0f;

    int64_t now = esp_timer_get_time();
    s_blink[0]  = { BLINK_IDLE, 0.0f, now };
    s_blink[1]  = { BLINK_IDLE, 0.0f, now };
    s_next_blink_us = now + poisson_blink_delay_us();

    esp_err_t err = render_service_register_layer(10, render_layer_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "render_service_register_layer falhou: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_set_mutex);
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "expression_service inicializado — modelo EMO, NEUTRAL, blink ativo");
    return ESP_OK;
}

void expression_service_set(nb_expression_t expr, float transition_ms)
{
    if ((int)expr < 0 || expr >= NB_EXPR_COUNT) {
        ESP_LOGW(TAG, "expression_service_set: expr=%d invalida", (int)expr);
        return;
    }

    xSemaphoreTake(s_set_mutex, portMAX_DELAY);
    s_pending_base_expr  = expr;
    s_pending_target     = NB_EXPRESSIONS[expr];
    s_pending_trans_ms   = transition_ms < 0.0f ? 0.0f : transition_ms;
    s_new_target_pending = true;
    xSemaphoreGive(s_set_mutex);

    ESP_LOGD(TAG, "set expr=%d trans=%.0fms", (int)expr, (double)transition_ms);
}

esp_err_t expression_play(nb_expression_t expr,
                          float            duration_ms,
                          float            transition_ms)
{
    if ((int)expr < 0 || expr >= NB_EXPR_COUNT) {
        ESP_LOGW(TAG, "expression_play: expr=%d invalida", (int)expr);
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_set_mutex, portMAX_DELAY);
    if (s_play_count >= PLAY_QUEUE_CAP) {
        xSemaphoreGive(s_set_mutex);
        ESP_LOGW(TAG, "expression_play: fila cheia");
        return ESP_ERR_NO_MEM;
    }
    int tail = (s_play_head + s_play_count) % PLAY_QUEUE_CAP;
    s_play_queue[tail] = {
        .expr        = expr,
        .duration_ms = duration_ms < 0.0f ? 0.0f : duration_ms,
        .trans_ms    = transition_ms < 0.0f ? 0.0f : transition_ms,
    };
    s_play_count++;
    xSemaphoreGive(s_set_mutex);

    ESP_LOGD(TAG, "play expr=%d dur=%.0fms tr=%.0fms (queue=%d)",
             (int)expr, (double)duration_ms, (double)transition_ms, s_play_count);
    return ESP_OK;
}

void expression_service_get_current(nb_face_state_t *out)
{
    if (out) *out = s_current;
}

void expression_service_set_gaze(float x, float y)
{
    s_gaze_x = x;
    s_gaze_y = y;
}

void expression_service_set_idle_overlay(float dy_l, float dy_r,
                                         float dopen_l, float dopen_r)
{
    /* Clamp defensivo para evitar overlays absurdos por bug de chamador. */
    const float MAX_DY    = 0.30f;
    const float MAX_DOPEN = 0.30f;
    if (dy_l    >  MAX_DY)    dy_l    =  MAX_DY;
    if (dy_l    < -MAX_DY)    dy_l    = -MAX_DY;
    if (dy_r    >  MAX_DY)    dy_r    =  MAX_DY;
    if (dy_r    < -MAX_DY)    dy_r    = -MAX_DY;
    if (dopen_l >  MAX_DOPEN) dopen_l =  MAX_DOPEN;
    if (dopen_l < -MAX_DOPEN) dopen_l = -MAX_DOPEN;
    if (dopen_r >  MAX_DOPEN) dopen_r =  MAX_DOPEN;
    if (dopen_r < -MAX_DOPEN) dopen_r = -MAX_DOPEN;
    s_idle_dy_l    = dy_l;
    s_idle_dy_r    = dy_r;
    s_idle_dopen_l = dopen_l;
    s_idle_dopen_r = dopen_r;
}

void expression_service_set_idle_rotation(float rot_l, float rot_r)
{
    const float MAX_ROT = 45.0f;
    if (rot_l >  MAX_ROT) rot_l =  MAX_ROT;
    if (rot_l < -MAX_ROT) rot_l = -MAX_ROT;
    if (rot_r >  MAX_ROT) rot_r =  MAX_ROT;
    if (rot_r < -MAX_ROT) rot_r = -MAX_ROT;
    s_idle_rot_l = rot_l;
    s_idle_rot_r = rot_r;
}

void nb_face_state_lerp(const nb_face_state_t *a,
                        const nb_face_state_t *b,
                        float t,
                        nb_face_state_t *out)
{
    if (!a || !b || !out) return;

    out->tl_l    = lerpf(a->tl_l,    b->tl_l,    t);
    out->tr_l    = lerpf(a->tr_l,    b->tr_l,    t);
    out->bl_l    = lerpf(a->bl_l,    b->bl_l,    t);
    out->br_l    = lerpf(a->br_l,    b->br_l,    t);
    out->tl_r    = lerpf(a->tl_r,    b->tl_r,    t);
    out->tr_r    = lerpf(a->tr_r,    b->tr_r,    t);
    out->bl_r    = lerpf(a->bl_r,    b->bl_r,    t);
    out->br_r    = lerpf(a->br_r,    b->br_r,    t);
    out->open_l  = lerpf(a->open_l,  b->open_l,  t);
    out->open_r  = lerpf(a->open_r,  b->open_r,  t);
    out->y_l     = lerpf(a->y_l,     b->y_l,     t);
    out->y_r     = lerpf(a->y_r,     b->y_r,     t);
    out->x_off   = lerpf(a->x_off,   b->x_off,   t);
    out->rt_top  = lerpf(a->rt_top,  b->rt_top,  t);
    out->rb_bot  = lerpf(a->rb_bot,  b->rb_bot,  t);
    out->cv_top  = lerpf(a->cv_top,  b->cv_top,  t);
    out->cv_bot  = lerpf(a->cv_bot,  b->cv_bot,  t);
    out->squint_l = lerpf(a->squint_l, b->squint_l, t);
    out->squint_r = lerpf(a->squint_r, b->squint_r, t);

    /* color: step na metade da transição */
    out->color   = (t < 0.5f) ? a->color : b->color;
}

void expression_combo_play(const nb_expr_frame_t *frames, uint8_t count)
{
    if (!frames) return;
    uint8_t n = (count > 4u) ? 4u : count;
    for (uint8_t i = 0; i < n; i++) {
        expression_play(frames[i].expr, frames[i].duration_ms, frames[i].transition_ms);
    }
}

void expression_service_play_wake_sequence(void)
{
    if (!s_initialized) return;
    xSemaphoreTake(s_set_mutex, portMAX_DELAY);
    s_wake_seq_pending = true;
    xSemaphoreGive(s_set_mutex);
}

void expression_service_overlay_blush(uint8_t intensity, uint32_t duration_ms)
{
    if (!s_initialized || duration_ms == 0U || intensity == 0U) return;
    xSemaphoreTake(s_set_mutex, portMAX_DELAY);
    s_blush_overlay.active      = true;
    s_blush_overlay.intensity   = intensity;
    s_blush_overlay.duration_ms = duration_ms;
    s_blush_overlay.start_us    = esp_timer_get_time();
    xSemaphoreGive(s_set_mutex);
}

void expression_service_overlay_heart(uint32_t duration_ms)
{
    if (!s_initialized || duration_ms == 0U) return;
    xSemaphoreTake(s_set_mutex, portMAX_DELAY);
    s_heart_overlay.active      = true;
    s_heart_overlay.duration_ms = duration_ms;
    s_heart_overlay.start_us    = esp_timer_get_time();
    xSemaphoreGive(s_set_mutex);
}

void expression_service_set_breath_enabled(bool enabled)
{
    s_breath_enabled = enabled;
}

void expression_service_set_blink_enabled(bool enabled)
{
    s_blink_enabled = enabled;
}

void expression_service_set_sleep_anim_enabled(bool enabled)
{
    if (enabled && !s_sleep_anim_enabled) {
        s_sleep_anim_start_us = esp_timer_get_time();
    }
    s_sleep_anim_enabled = enabled;
}

void expression_service_set_speaking_mouth_enabled(bool enabled)
{
    if (enabled && !s_speaking_mouth_enabled) {
        s_speaking_mouth_start_us = esp_timer_get_time();
    }
    s_speaking_mouth_enabled = enabled;
}

} /* extern "C" */
