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
 *   - Boca e sobrancelhas: fora deste módulo — peças ocasionais de Layer 5+.
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
#include "display_lgfx_config.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cmath>

#define TAG "nb_expr"

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
static constexpr float GAZE_Y_MAX        = 0.18f; /* mantém gaze perto do centro */

/* ── Blink ───────────────────────────────────────────────────────────────── */

static constexpr float   BLINK_MEAN_MS  = 4200.0f;
static constexpr float   BLINK_MIN_MS   = 1600.0f;
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
static constexpr int FACE_DIRTY_X1 = (int)BASE_R_CX  + (int)HW_I + (int)X_OFF_TRAVEL + GAZE_X_MARGIN + (int)BLINK_BAR_EXTRA_HW;
static constexpr int FACE_DIRTY_Y0 = (int)EYE_CY_BASE - (int)MAX_HH_F - (int)Y_TRAVEL_PX - (int)MAX_CURVE_PX - 2;
static constexpr int FACE_DIRTY_Y1 = (int)EYE_CY_BASE + (int)MAX_HH_F + (int)Y_TRAVEL_PX + (int)MAX_CURVE_PX + 16;

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

/* Dois olhos independentes para blink assimétrico */
static nb_blink_eye_t     s_blink[2]          = {};
static int64_t            s_next_blink_us     = 0;

/*
 * Gaze offset — escrito por gaze_render_cb (z=5) e lido por este callback
 * (z=10), ambos no mesmo Core 1 render_task, sequencialmente no mesmo frame.
 * Não são volatile: volatile não provê barrier cross-core no Xtensa, e o
 * acesso é single-core. Nenhuma outra task deve chamar set_gaze diretamente.
 */
static float              s_gaze_x            = 0.0f;
static float              s_gaze_y            = 0.0f;

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
static bool               s_blink_prev_enabled = true;

static constexpr float BREATH_PERIOD_MS = 5200.0f;
static constexpr float BREATH_AMP       = 0.045f;
static constexpr float NB_PI_F          = 3.14159265358979323846f;

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
        .open_l=0.86f, .open_r=0.86f,
        .y_l=0.00f,    .y_r=0.00f,
        .x_off=0.00f,
        .rt_top=0.38f, .rb_bot=0.38f,
        .cv_top=0.50f, .cv_bot=0.50f,
        .color=TFT_WHITE,
        .squint_l=0.00f, .squint_r=0.00f,
    },



    /* HAPPY — arco forte na base, squint suave: olhos em meia-lua */
    {
        .tl_l=0.00f,.tr_l=0.00f,.bl_l=0.72f,.br_l=0.72f,
        .tl_r=0.00f,.tr_r=0.00f,.bl_r=0.72f,.br_r=0.72f,
        .open_l=0.41f, .open_r=0.41f,
        .y_l=0.00f,    .y_r=0.00f,
        .x_off=0.00f,
        .rt_top=0.27f, .rb_bot=0.52f,
        .cv_top=1.00f, .cv_bot=-1.00f,
        .color=TFT_WHITE,
        .squint_l=0.22f, .squint_r=0.22f,
    },

    /* CURIOUS — olho direito mais aberto e mais alto, curvatura no topo forte */
    {
        .tl_l=0.00f,.tr_l=0.00f,.bl_l=0.00f,.br_l=0.00f,
        .tl_r=0.10f,.tr_r=0.00f,.bl_r=0.00f,.br_r=0.00f,
        .open_l=0.82f, .open_r=1.00f,
        .y_l=0.00f,    .y_r=-0.28f,
        .x_off=0.08f,
        .rt_top=0.42f, .rb_bot=0.32f,
        .cv_top=0.65f, .cv_bot=0.10f,
        .color=TFT_WHITE,
        .squint_l=0.00f, .squint_r=0.00f,
    },

    /* SLEEPY — abertura baixa, squint pesado, olhos levemente descidos */
    {
        .tl_l=0.16f,.tr_l=0.00f,.bl_l=0.00f,.br_l=0.00f,
        .tl_r=0.00f,.tr_r=0.16f,.bl_r=0.00f,.br_r=0.00f,
        .open_l=0.10f, .open_r=0.10f,
        .y_l=1.00f,    .y_r=1.00f,
        .x_off=0.00f,
        .rt_top=0.28f, .rb_bot=0.28f,
        .cv_top=0.43f, .cv_bot=-0.25f,
        .color=TFT_WHITE,
        .squint_l=0.51f, .squint_r=0.51f,
    },


    /* FOCUSED — cantos internos do topo descidos, squint leve: olhar concentrado */
    {
        .tl_l=0.00f,.tr_l=0.30f,.bl_l=0.00f,.br_l=0.00f,
        .tl_r=0.30f,.tr_r=0.00f,.bl_r=0.00f,.br_r=0.00f,
        .open_l=0.80f, .open_r=0.80f,
        .y_l=0.00f,    .y_r=0.00f,
        .x_off=0.00f,
        .rt_top=0.35f, .rb_bot=0.35f,
        .cv_top=0.30f, .cv_bot=0.05f,
        .color=TFT_WHITE,
        .squint_l=0.10f, .squint_r=0.10f,
    },

    /* SUSPICIOUS — cantos internos fortemente descidos, squint médio: V agressivo */
    {
        .tl_l=0.00f,.tr_l=0.62f,.bl_l=0.00f,.br_l=0.00f,
        .tl_r=0.62f,.tr_r=0.00f,.bl_r=0.00f,.br_r=0.00f,
        .open_l=0.60f, .open_r=0.60f,
        .y_l=0.10f,    .y_r=0.10f,
        .x_off=0.00f,
        .rt_top=0.20f, .rb_bot=0.28f,
        .cv_top=-0.15f,.cv_bot=0.05f,
        .color=TFT_WHITE,
        .squint_l=0.38f, .squint_r=0.38f,
    },

    /* SURPRISED — olhos arregalados: open > 1.0, cantos abertos, sem squint */
    {
        .tl_l=0.00f,.tr_l=0.00f,.bl_l=0.00f,.br_l=0.00f,
        .tl_r=0.00f,.tr_r=0.00f,.bl_r=0.00f,.br_r=0.00f,
        .open_l=1.10f, .open_r=1.10f,
        .y_l=0.00f,    .y_r=0.00f,
        .x_off=0.00f,
        .rt_top=0.45f, .rb_bot=0.45f,
        .cv_top=0.60f, .cv_bot=0.20f,
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
        .tl_l=0.55f,.tr_l=0.00f,.bl_l=0.00f,.br_l=0.20f,
        .tl_r=0.00f,.tr_r=0.55f,.bl_r=0.20f,.br_r=0.00f,
        .open_l=0.68f, .open_r=0.68f,
        .y_l=0.20f,    .y_r=0.20f,
        .x_off=0.00f,
        .rt_top=0.28f, .rb_bot=0.32f,
        .cv_top=-0.10f,.cv_bot=0.08f,
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
        .open_l=0.95f, .open_r=0.95f,
        .y_l=-0.18f,   .y_r=-0.18f,
        .x_off=0.04f,
        .rt_top=0.42f, .rb_bot=0.35f,
        .cv_top=0.55f, .cv_bot=0.10f,
        .color=TFT_WHITE,
        .squint_l=0.00f, .squint_r=0.00f,
    },
};

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
    if (fabsf(gx) > 0.03f) {
        return gy * 0.25f;
    }
    return gy;
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

static void draw_blush_overlay(LGFX_Sprite *spr, int64_t now_us)
{
    if (!s_blush_overlay.active) return;

    float alpha = overlay_alpha(now_us, s_blush_overlay.start_us,
                                s_blush_overlay.duration_ms);
    if (alpha <= 0.0f) {
        s_blush_overlay.active = false;
        return;
    }

    alpha *= (float)s_blush_overlay.intensity / 255.0f;
    uint32_t c1 = blend_with_black(0xFF6FA3u, alpha * 0.42f);
    uint32_t c2 = blend_with_black(0xFF8AB5u, alpha * 0.26f);
    int16_t y = 166;

    spr->fillEllipse(80,  y, 25, 9, c1);
    spr->fillEllipse(240, y, 25, 9, c1);
    spr->fillEllipse(80,  y, 16, 5, c2);
    spr->fillEllipse(240, y, 16, 5, c2);
}

static void draw_heart_overlay(LGFX_Sprite *spr, int64_t now_us)
{
    if (!s_heart_overlay.active) return;

    float alpha = overlay_alpha(now_us, s_heart_overlay.start_us,
                                s_heart_overlay.duration_ms);
    if (alpha <= 0.0f) {
        s_heart_overlay.active = false;
        return;
    }

    int64_t elapsed_us = now_us - s_heart_overlay.start_us;
    float elapsed_ms = elapsed_us > 0 ? (float)elapsed_us / 1000.0f : 0.0f;
    float pulse = 1.0f + 0.10f * sinf(elapsed_ms * 0.018f);
    int16_t r = (int16_t)(11.0f * pulse);
    int16_t cx = 160;
    int16_t cy = 166;
    uint32_t color = blend_with_black(0xFF4F7Bu, alpha);

    spr->fillCircle(cx - r, cy - 3, r, color);
    spr->fillCircle(cx + r, cy - 3, r, color);
    spr->fillTriangle(cx - (r * 2), cy + 1,
                      cx + (r * 2), cy + 1,
                      cx,           cy + (r * 3), color);
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
                /* Próximo blink agendado a partir do momento em que o olho esquerdo abre */
                if (is_left) {
                    s_next_blink_us = now_us + poisson_blink_delay_us();
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
        s_blink_prev_enabled = false;
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
}

/* ── Renderer do olho EMO ────────────────────────────────────────────────── */

/*
 * Desenha um olho no estilo EMO: quadrilátero paramétrico com curvatura,
 * arredondamento de cantos e squint da pálpebra superior.
 *
 * @param spr        Canvas LGFX_Sprite de destino.
 * @param base_cx    Centro X base do olho (antes do x_off).
 * @param y_off      Offset vertical normalizado [-1..1].
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
                         int16_t base_cx, float y_off,
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

    float cy_f  = (float)EYE_CY_BASE + y_off * Y_TRAVEL_PX;
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

    if (s_new_target_pending) {
        if (xSemaphoreTake(s_set_mutex, 0) == pdTRUE) {
            if (s_new_target_pending) {
                s_base_expr          = s_pending_base_expr;
                s_from               = s_current;
                s_target             = s_pending_target;
                s_trans_total_ms     = s_pending_trans_ms;
                s_trans_elapsed_ms   = 0.0f;
                s_new_target_pending = false;
                s_play_state         = PLAY_STATE_IDLE;   /* cancela play ativo */
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

    /* Interpolação de estado */
    if (s_trans_elapsed_ms < s_trans_total_ms && s_trans_total_ms > 0.0f) {
        s_trans_elapsed_ms += FRAME_MS;
        float t = s_trans_elapsed_ms / s_trans_total_ms;
        if (t > 1.0f) t = 1.0f;
        nb_face_state_lerp(&s_from, &s_target, t, &s_current);
    } else {
        s_current = s_target;
    }

    /* Blink (atualiza fase de cada olho) */
    blink_update(now_us);

    nb_face_state_t face = s_current;
    if (s_breath_enabled) {
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

    /* Centros X dos olhos com x_off (convergência) e gaze_x (translation) aplicados */
    float   gx        = s_gaze_x;
    float   gy        = damp_vertical_for_lateral_gaze(gx, clamp_abs(s_gaze_y, GAZE_Y_MAX));
    int16_t gaze_shift = (int16_t)(gx * GAZE_X_TRAVEL_PX + (gx >= 0.0f ? 0.5f : -0.5f));
    int16_t left_cx   = BASE_L_CX
                      + (int16_t)(face.x_off * X_OFF_TRAVEL + 0.5f)
                      + gaze_shift;
    int16_t right_cx  = BASE_R_CX
                      - (int16_t)(face.x_off * X_OFF_TRAVEL + 0.5f)
                      + gaze_shift;

    /* Offsets Y combinados com gaze_y */
    float y_l = clamp_abs(face.y_l + gy, GAZE_Y_MAX);
    float y_r = clamp_abs(face.y_r + gy, GAZE_Y_MAX);

    /* Canvas já limpo em TFT_BLACK pelo render_service */

    /*
     * Blink bar EMO: quando qualquer olho entra na fase de barra, os dois olhos
     * são substituídos por uma barra única que vai da borda externa do olho
     * esquerdo à borda externa do olho direito (+ padding). Mais larga que
     * cada olho individual — visual unificado característico do estilo EMO.
     */
    if (s_blink[0].phase > BLINK_BAR_PH_THRESH ||
        s_blink[1].phase > BLINK_BAR_PH_THRESH) {

        float   bar_cy_f = (float)EYE_CY_BASE
                         + (y_l + y_r) * 0.5f * Y_TRAVEL_PX;
        int16_t bar_cy   = (int16_t)(bar_cy_f + 0.5f);
        int16_t bx       = left_cx  - HW_I - BLINK_BAR_EXTRA_HW;
        int16_t bw       = (right_cx + HW_I + BLINK_BAR_EXTRA_HW) - bx;

        spr->drawFastHLine(bx, bar_cy - 1, bw, face.color);
        spr->drawFastHLine(bx, bar_cy,     bw, face.color);
        spr->drawFastHLine(bx, bar_cy + 1, bw, face.color);

    } else {

        /* Olho esquerdo */
        draw_emo_eye(spr,
                     left_cx, y_l,
                     face.open_l,
                     face.tl_l, face.tr_l,
                     face.bl_l, face.br_l,
                     face.squint_l,
                     face.rt_top, face.rb_bot,
                     face.cv_top, face.cv_bot,
                     s_blink[0].phase,
                     face.color);

        /* Olho direito — tl_r/bl_r = lados internos, tr_r/br_r = externos. */
        draw_emo_eye(spr,
                     right_cx, y_r,
                     face.open_r,
                     face.tl_r, face.tr_r,
                     face.bl_r, face.br_r,
                     face.squint_r,
                     face.rt_top, face.rb_bot,
                     face.cv_top, face.cv_bot,
                     s_blink[1].phase,
                     face.color);
    }

    draw_blush_overlay(spr, now_us);
    draw_heart_overlay(spr, now_us);

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

    s_current          = NB_EXPRESSIONS[NB_EXPR_NEUTRAL];
    s_target           = NB_EXPRESSIONS[NB_EXPR_NEUTRAL];
    s_from             = NB_EXPRESSIONS[NB_EXPR_NEUTRAL];
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

} /* extern "C" */
