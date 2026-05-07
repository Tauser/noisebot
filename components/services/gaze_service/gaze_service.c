/*
 * gaze_service.c — Gaze system do NoiseBot (Layer 5)
 *
 * Registra um render layer (z=5) que roda antes do expression layer (z=10).
 * A cada frame (~33ms):
 *   1. Consome target/glance pendente (se houver) → inicia fase rápida.
 *   2. Avança a máquina de estado:
 *        set_target: FAST → SETTLE → DRIFT
 *        glance:     FAST → SETTLE → HOLD → RETURN_FAST → RETURN_SETTLE → DRIFT
 *   3. Aplica micro-drift gaussiano low-pass no estado DRIFT.
 *   4. Chama expression_service_set_gaze() com a posição atual.
 *
 * Não desenha nada no canvas — o layer é usado apenas para timing 30fps.
 *
 * Thread safety:
 *   - gaze_service_set_target/glance(): protegidos por s_mux (portMUX spinlock).
 *   - gaze_service_set_anchor(): protegido por s_anchor_mux.
 *   - s_cur_x/y, s_drift_x/y: escritos somente em render_task (Core 1);
 *     leituras de outros cores são best-effort (sem decisão crítica).
 */

#include "gaze_service.h"
#include "expression_service.h"
#include "render_service.h"
#include "attention_service.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_log.h"
#include "esp_random.h"

#define TAG "nb_gaze"

/* ── Parâmetros ──────────────────────────────────────────────────────────── */

#define SACCADE_FAST_MS      60.0f
#define SACCADE_SETTLE_MS   150.0f
#define SACCADE_OVERSHOOT     0.10f

#define DRIFT_STEP            0.0025f  /* drift visível sem empurrar aos cantos */
#define DRIFT_MAX_R           0.060f   /* raio contido para manter centro visual */
#define DRIFT_LP              0.18f

#define GAZE_MAX              0.65f
#define FRAME_MS              33.3f

/* ── Fases do saccade ────────────────────────────────────────────────────── */

typedef enum {
    GAZE_DRIFT,          /* apenas micro-drift, sem saccade ativo       */
    GAZE_FAST,           /* fase rápida: current → target+overshoot     */
    GAZE_SETTLE,         /* fase de settle: overshoot → target          */
    GAZE_HOLD,           /* segura no target (glance hold)              */
    GAZE_RETURN_FAST,    /* fase rápida de retorno ao âncora            */
    GAZE_RETURN_SETTLE,  /* fase de settle de retorno ao âncora         */
} gaze_phase_t;

/* ── Estado interno ──────────────────────────────────────────────────────── */

static float        s_cur_x      = 0.0f;
static float        s_cur_y      = 0.0f;

static float        s_tgt_x      = 0.0f;
static float        s_tgt_y      = 0.0f;
static float        s_start_x    = 0.0f;
static float        s_start_y    = 0.0f;
static float        s_over_x     = 0.0f;
static float        s_over_y     = 0.0f;

static float        s_phase_ms   = 0.0f;
static gaze_phase_t s_phase      = GAZE_DRIFT;

static float        s_drift_x    = 0.0f;
static float        s_drift_y    = 0.0f;

/* Glance: hold e flag de retorno automático */
static bool         s_is_glance         = false;
static float        s_hold_remaining_ms = 0.0f;

/* Âncora de retorno após glance (padrão centro) */
static float        s_anchor_x   = 0.0f;
static float        s_anchor_y   = 0.0f;
static portMUX_TYPE s_anchor_mux = portMUX_INITIALIZER_UNLOCKED;

/* Target/glance pendente (escrito de qualquer task, consumido no render Core 1) */
static volatile bool s_new_target      = false;
static float         s_pending_x       = 0.0f;
static float         s_pending_y       = 0.0f;
static bool          s_pending_glance  = false;
static uint32_t      s_pending_hold_ms = 0;
static portMUX_TYPE  s_mux             = portMUX_INITIALIZER_UNLOCKED;

static bool s_initialized = false;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

/* ease-out quadrático: começa rápido, desacelera no final */
static inline float ease_out(float t)
{
    float inv = 1.0f - t;
    return 1.0f - inv * inv;
}

/* Ruído uniforme em [-1, 1] via hardware RNG */
static inline float rand11(void)
{
    return (float)(int32_t)esp_random() / 2147483648.0f;
}

/* ── Render layer callback (z=5, Core 1, ~30fps) ────────────────────────── */

static void gaze_render_cb(nb_display_sprite_t canvas, void *ctx)
{
    (void)canvas;
    (void)ctx;

    /* 1. Consumir target/glance pendente */
    if (s_new_target) {
        taskENTER_CRITICAL(&s_mux);
        float    tx      = s_pending_x;
        float    ty      = s_pending_y;
        bool     is_glnc = s_pending_glance;
        float    hold_ms = (float)s_pending_hold_ms;
        s_new_target = false;
        taskEXIT_CRITICAL(&s_mux);

        float dx = tx - s_cur_x;
        float dy = ty - s_cur_y;
        s_over_x = clampf(tx + dx * SACCADE_OVERSHOOT, -GAZE_MAX, GAZE_MAX);
        s_over_y = clampf(ty + dy * SACCADE_OVERSHOOT, -GAZE_MAX, GAZE_MAX);

        s_tgt_x    = tx;
        s_tgt_y    = ty;
        s_start_x  = s_cur_x;
        s_start_y  = s_cur_y;
        s_phase    = GAZE_FAST;
        s_phase_ms = 0.0f;

        /* Zera drift ao iniciar saccade — evita carry-through nas bordas */
        s_drift_x = 0.0f;
        s_drift_y = 0.0f;

        s_is_glance         = is_glnc;
        s_hold_remaining_ms = hold_ms;
    }

    /* 2. Avançar fase */
    s_phase_ms += FRAME_MS;

    switch (s_phase) {

        case GAZE_FAST: {
            /* Velocidade proporcional à atenção: [attention=0 → 1.5×lento, attention=1 → 0.5×rápido] */
            float attn    = attention_service_get_level();
            float fast_ms = SACCADE_FAST_MS * (1.5f - attn);
            float t = s_phase_ms / fast_ms;
            if (t >= 1.0f) {
                t = 1.0f;
                s_start_x  = s_over_x;
                s_start_y  = s_over_y;
                s_phase    = GAZE_SETTLE;
                s_phase_ms = 0.0f;
            }
            s_cur_x = lerpf(s_start_x, s_over_x, t);
            s_cur_y = lerpf(s_start_y, s_over_y, t);
            break;
        }

        case GAZE_SETTLE: {
            float t = ease_out(s_phase_ms / SACCADE_SETTLE_MS);
            if (t >= 1.0f) {
                t = 1.0f;
                /* Glance → aguarda hold; set_target → deriva normalmente */
                s_phase    = s_is_glance ? GAZE_HOLD : GAZE_DRIFT;
                s_phase_ms = 0.0f;
            }
            s_cur_x = lerpf(s_start_x, s_tgt_x, t);
            s_cur_y = lerpf(s_start_y, s_tgt_y, t);
            break;
        }

        case GAZE_HOLD: {
            /* Olhos parados no target durante o hold */
            s_cur_x = s_tgt_x;
            s_cur_y = s_tgt_y;
            if (s_hold_remaining_ms <= FRAME_MS) {
                /* Hold expirado — inicia saccade de retorno ao âncora */
                taskENTER_CRITICAL(&s_anchor_mux);
                float ax = s_anchor_x;
                float ay = s_anchor_y;
                taskEXIT_CRITICAL(&s_anchor_mux);

                float dx = ax - s_cur_x;
                float dy = ay - s_cur_y;
                s_over_x = clampf(ax + dx * SACCADE_OVERSHOOT, -GAZE_MAX, GAZE_MAX);
                s_over_y = clampf(ay + dy * SACCADE_OVERSHOOT, -GAZE_MAX, GAZE_MAX);
                s_tgt_x    = ax;
                s_tgt_y    = ay;
                s_start_x  = s_cur_x;
                s_start_y  = s_cur_y;
                s_phase    = GAZE_RETURN_FAST;
                s_phase_ms = 0.0f;
                s_drift_x  = 0.0f;
                s_drift_y  = 0.0f;
            } else {
                s_hold_remaining_ms -= FRAME_MS;
            }
            break;
        }

        case GAZE_RETURN_FAST: {
            float attn    = attention_service_get_level();
            float fast_ms = SACCADE_FAST_MS * (1.5f - attn);
            float t = s_phase_ms / fast_ms;
            if (t >= 1.0f) {
                t = 1.0f;
                s_start_x  = s_over_x;
                s_start_y  = s_over_y;
                s_phase    = GAZE_RETURN_SETTLE;
                s_phase_ms = 0.0f;
            }
            s_cur_x = lerpf(s_start_x, s_over_x, t);
            s_cur_y = lerpf(s_start_y, s_over_y, t);
            break;
        }

        case GAZE_RETURN_SETTLE: {
            float t = ease_out(s_phase_ms / SACCADE_SETTLE_MS);
            if (t >= 1.0f) {
                t = 1.0f;
                s_phase     = GAZE_DRIFT;
                s_is_glance = false;
            }
            s_cur_x = lerpf(s_start_x, s_tgt_x, t);
            s_cur_y = lerpf(s_start_y, s_tgt_y, t);
            break;
        }

        case GAZE_DRIFT: {
            /* Passeio gaussiano low-pass */
            float raw_dx = rand11() * DRIFT_STEP;
            float raw_dy = rand11() * DRIFT_STEP;

            s_drift_x = lerpf(s_drift_x, s_drift_x + raw_dx, DRIFT_LP);
            s_drift_y = lerpf(s_drift_y, s_drift_y + raw_dy, DRIFT_LP);

            /* Atenuação elástica quando excede raio máximo */
            float dist2 = s_drift_x * s_drift_x + s_drift_y * s_drift_y;
            if (dist2 > DRIFT_MAX_R * DRIFT_MAX_R) {
                s_drift_x *= 0.88f;
                s_drift_y *= 0.88f;
            }

            s_cur_x = s_tgt_x + s_drift_x;
            s_cur_y = s_tgt_y + s_drift_y;
            break;
        }
    }

    s_cur_x = clampf(s_cur_x, -GAZE_MAX, GAZE_MAX);
    s_cur_y = clampf(s_cur_y, -GAZE_MAX, GAZE_MAX);

    /* 3. Empurrar offset para expression_service (lido no frame atual, z=10) */
    expression_service_set_gaze(s_cur_x, s_cur_y);
}

/* ── API ─────────────────────────────────────────────────────────────────── */

esp_err_t gaze_service_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;

    esp_err_t err = render_service_register_layer(5, gaze_render_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register_layer falhou: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "gaze_service inicializado (render layer z=5)");
    return ESP_OK;
}

void gaze_service_set_target(float x, float y)
{
    taskENTER_CRITICAL(&s_mux);
    s_pending_x      = clampf(x, -GAZE_MAX, GAZE_MAX);
    s_pending_y      = clampf(y, -GAZE_MAX, GAZE_MAX);
    s_pending_glance = false;
    s_new_target     = true;
    taskEXIT_CRITICAL(&s_mux);
}

void gaze_service_glance(float x, float y, uint32_t hold_ms)
{
    taskENTER_CRITICAL(&s_mux);
    s_pending_x       = clampf(x, -GAZE_MAX, GAZE_MAX);
    s_pending_y       = clampf(y, -GAZE_MAX, GAZE_MAX);
    s_pending_glance  = true;
    s_pending_hold_ms = hold_ms;
    s_new_target      = true;
    taskEXIT_CRITICAL(&s_mux);
}

void gaze_service_set_anchor(float x, float y)
{
    taskENTER_CRITICAL(&s_anchor_mux);
    s_anchor_x = clampf(x, -GAZE_MAX, GAZE_MAX);
    s_anchor_y = clampf(y, -GAZE_MAX, GAZE_MAX);
    taskEXIT_CRITICAL(&s_anchor_mux);
}

void gaze_service_get_current(float *x, float *y)
{
    if (x) *x = s_cur_x;
    if (y) *y = s_cur_y;
}
