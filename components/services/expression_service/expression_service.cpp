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

static constexpr float Y_TRAVEL_PX    = 14.0f;   /* pixels para y=±1      */
static constexpr float X_OFF_TRAVEL   = 18.0f;   /* pixels para x_off=±1  */
static constexpr float MAX_CURVE_PX   = 10.0f;   /* pixels de curvatura máx */

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

/* ── Estado interno ──────────────────────────────────────────────────────── */

static bool               s_initialized      = false;
static SemaphoreHandle_t  s_set_mutex        = NULL;

static nb_face_state_t    s_current          = {};
static nb_face_state_t    s_target           = {};
static nb_face_state_t    s_from             = {};
static float              s_trans_total_ms   = 0.0f;
static float              s_trans_elapsed_ms = 0.0f;

static volatile bool      s_new_target_pending = false;
static nb_face_state_t    s_pending_target     = {};
static float              s_pending_trans_ms   = 0.0f;

/* Dois olhos independentes para blink assimétrico */
static nb_blink_eye_t     s_blink[2]          = {};
static int64_t            s_next_blink_us     = 0;

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
        .tl_l=0.00f,.tr_l=0.00f,.bl_l=0.00f,.br_l=0.00f,
        .tl_r=0.00f,.tr_r=0.00f,.bl_r=0.00f,.br_r=0.00f,
        .open_l=0.28f, .open_r=0.28f,
        .y_l=0.22f,    .y_r=0.22f,
        .x_off=0.00f,
        .rt_top=0.28f, .rb_bot=0.28f,
        .cv_top=0.10f, .cv_bot=0.05f,
        .color=TFT_WHITE,
        .squint_l=0.52f, .squint_r=0.52f,
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
        .open_l=1.30f, .open_r=1.30f,
        .y_l=0.00f,    .y_r=0.00f,
        .x_off=0.00f,
        .rt_top=0.45f, .rb_bot=0.45f,
        .cv_top=0.60f, .cv_bot=0.20f,
        .color=TFT_WHITE,
        .squint_l=0.00f, .squint_r=0.00f,
    },

    /* SAD — placeholder, cópia de SUSPICIOUS. Ajustar. */
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

    /* ALARMED — placeholder, cópia de SUSPICIOUS. Ajustar. */
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
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static inline float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
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
    s_blink[0].t0    = now_us;
    s_blink[1].state = BLINK_CLOSING;

    uint32_t rnd = esp_random();
    if ((rnd & 0xFFu) < BLINK_ASYM_THRESH) {
        /* Assimetria: olho direito começa ligeiramente atrasado */
        int64_t delay = BLINK_ASYM_MIN_US
                      + (int64_t)(((rnd >> 8) & 0x7Fu) * (BLINK_ASYM_RANGE / 128LL));
        s_blink[1].t0 = now_us + delay;
    } else {
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
            eye->phase = clamp01((float)dt / (float)BLINK_CLOSE_US);
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
            eye->phase = 1.0f - clamp01((float)dt / (float)BLINK_OPEN_US);
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

    /* Olho praticamente fechado: linha de blink */
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

    /* Aplicar pedido externo de expressão (trylock — não bloqueia render_task) */
    if (s_new_target_pending) {
        if (xSemaphoreTake(s_set_mutex, 0) == pdTRUE) {
            if (s_new_target_pending) {
                s_from             = s_current;
                s_target           = s_pending_target;
                s_trans_total_ms   = s_pending_trans_ms;
                s_trans_elapsed_ms = 0.0f;
                s_new_target_pending = false;
            }
            xSemaphoreGive(s_set_mutex);
        }
    }

    /* Interpolação de estado */
    if (s_trans_elapsed_ms < s_trans_total_ms && s_trans_total_ms > 0.0f) {
        s_trans_elapsed_ms += 33.3f;   /* ~1 frame a 30fps */
        float t = s_trans_elapsed_ms / s_trans_total_ms;
        if (t > 1.0f) t = 1.0f;
        nb_face_state_lerp(&s_from, &s_target, t, &s_current);
    } else {
        s_current = s_target;
    }

    /* Blink (atualiza fase de cada olho) */
    blink_update(now_us);

    /* Centros X dos olhos com x_off aplicado */
    int16_t left_cx  = BASE_L_CX + (int16_t)(s_current.x_off * X_OFF_TRAVEL + 0.5f);
    int16_t right_cx = BASE_R_CX - (int16_t)(s_current.x_off * X_OFF_TRAVEL + 0.5f);

    /* Canvas já limpo em TFT_BLACK pelo render_service */

    /* Olho esquerdo */
    draw_emo_eye(spr,
                 left_cx, s_current.y_l,
                 s_current.open_l,
                 s_current.tl_l, s_current.tr_l,
                 s_current.bl_l, s_current.br_l,
                 s_current.squint_l,
                 s_current.rt_top, s_current.rb_bot,
                 s_current.cv_top, s_current.cv_bot,
                 s_blink[0].phase,
                 s_current.color);

    /* Olho direito — sem espelhamento de cantos.
     * tl_r/bl_r = lados esquerdo/interno (menor x) na perspectiva do observador.
     * tr_r/br_r = lados direito/externo.
     * Simetria de expressão: tl_l==tr_r (externos) e tr_l==tl_r (internos). */
    draw_emo_eye(spr,
                 right_cx, s_current.y_r,
                 s_current.open_r,
                 s_current.tl_r, s_current.tr_r,
                 s_current.bl_r, s_current.br_r,
                 s_current.squint_r,
                 s_current.rt_top, s_current.rb_bot,
                 s_current.cv_top, s_current.cv_bot,
                 s_blink[1].phase,
                 s_current.color);
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
    s_pending_target     = NB_EXPRESSIONS[expr];
    s_pending_trans_ms   = transition_ms < 0.0f ? 0.0f : transition_ms;
    s_new_target_pending = true;
    xSemaphoreGive(s_set_mutex);

    ESP_LOGD(TAG, "set expr=%d trans=%.0fms", (int)expr, (double)transition_ms);
}

void expression_service_get_current(nb_face_state_t *out)
{
    if (out) *out = s_current;
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

} /* extern "C" */
