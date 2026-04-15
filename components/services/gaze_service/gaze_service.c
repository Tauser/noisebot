/*
 * gaze_service.c — Gaze system do NoiseBot (Layer 5)
 *
 * Registra um render layer (z=5) que roda antes do expression layer (z=10).
 * A cada frame (~33ms):
 *   1. Consome target pendente (se houver) → inicia fase rápida de saccade.
 *   2. Avança a máquina de estado do saccade (FAST → SETTLE → DRIFT).
 *   3. Aplica micro-drift gaussiano low-pass no estado DRIFT.
 *   4. Chama expression_service_set_gaze() com a posição atual.
 *
 * Não desenha nada no canvas — o layer é usado apenas para timing 30fps.
 *
 * Thread safety:
 *   - gaze_service_set_target(): protegido por portMUX spinlock.
 *   - s_cur_x/y: escritos somente em render_task (Core 1); leituras de
 *     outros cores são best-effort (nenhuma decisão crítica depende delas).
 */

#include "gaze_service.h"
#include "expression_service.h"
#include "render_service.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_log.h"
#include "esp_random.h"

#define TAG "nb_gaze"

/* ── Parâmetros ──────────────────────────────────────────────────────────── */

/** Duração da fase rápida do saccade (ms). */
#define SACCADE_FAST_MS       60.0f

/** Duração da fase de settle (ms). */
#define SACCADE_SETTLE_MS    150.0f

/** Overshoot fracionário além do target (10%). */
#define SACCADE_OVERSHOOT      0.10f

/** Incremento máximo de micro-drift por frame (unidades normalizadas). */
#define DRIFT_STEP            0.003f

/** Raio máximo do micro-drift (mantido por atenção suave). */
#define DRIFT_MAX_R           0.06f

/** Coeficiente do filtro low-pass do drift (0=estático, 1=sem filtro). */
#define DRIFT_LP              0.18f

/** Amplitude máxima do gaze (impede olhos saírem da tela). */
#define GAZE_MAX              0.65f

/** Duração de frame estimada em ms (30fps). */
#define FRAME_MS              33.3f

/* ── Fases do saccade ────────────────────────────────────────────────────── */

typedef enum {
    GAZE_DRIFT,   /* apenas micro-drift, sem saccade ativo */
    GAZE_FAST,    /* fase rápida: current → target+overshoot */
    GAZE_SETTLE,  /* fase de settle: overshoot → target      */
} gaze_phase_t;

/* ── Estado interno ──────────────────────────────────────────────────────── */

static float        s_cur_x      = 0.0f;
static float        s_cur_y      = 0.0f;

static float        s_tgt_x      = 0.0f;  /* target final do saccade     */
static float        s_tgt_y      = 0.0f;

static float        s_start_x    = 0.0f;  /* posição no início da fase   */
static float        s_start_y    = 0.0f;

static float        s_over_x     = 0.0f;  /* ponto de overshoot          */
static float        s_over_y     = 0.0f;

static float        s_phase_ms   = 0.0f;  /* tempo decorrido na fase     */
static gaze_phase_t s_phase      = GAZE_DRIFT;

/* Micro-drift acumulado (low-pass) */
static float        s_drift_x    = 0.0f;
static float        s_drift_y    = 0.0f;

/* Target pendente (escrito de qualquer task, consumido no render Core 1) */
static volatile bool s_new_target = false;
static float         s_pending_x  = 0.0f;
static float         s_pending_y  = 0.0f;
static portMUX_TYPE  s_mux        = portMUX_INITIALIZER_UNLOCKED;

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

    /* 1. Consumir target pendente */
    if (s_new_target) {
        taskENTER_CRITICAL(&s_mux);
        float tx = s_pending_x;
        float ty = s_pending_y;
        s_new_target = false;
        taskEXIT_CRITICAL(&s_mux);

        /* Calcular overshoot na direção do movimento */
        float dx = tx - s_cur_x;
        float dy = ty - s_cur_y;
        s_over_x = clampf(tx + dx * SACCADE_OVERSHOOT, -GAZE_MAX, GAZE_MAX);
        s_over_y = clampf(ty + dy * SACCADE_OVERSHOOT, -GAZE_MAX, GAZE_MAX);

        s_tgt_x   = tx;
        s_tgt_y   = ty;
        s_start_x = s_cur_x;
        s_start_y = s_cur_y;
        s_phase   = GAZE_FAST;
        s_phase_ms = 0.0f;

        /* Reset drift ao iniciar saccade */
        s_drift_x = 0.0f;
        s_drift_y = 0.0f;
    }

    /* 2. Avançar fase */
    s_phase_ms += FRAME_MS;

    switch (s_phase) {

        case GAZE_FAST: {
            float t = s_phase_ms / SACCADE_FAST_MS;
            if (t >= 1.0f) {
                t = 1.0f;
                /* Inicia settle a partir do overshoot */
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
                s_phase = GAZE_DRIFT;
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
    s_pending_x  = clampf(x, -GAZE_MAX, GAZE_MAX);
    s_pending_y  = clampf(y, -GAZE_MAX, GAZE_MAX);
    s_new_target = true;
    taskEXIT_CRITICAL(&s_mux);
}

void gaze_service_get_current(float *x, float *y)
{
    if (x) *x = s_cur_x;
    if (y) *y = s_cur_y;
}
