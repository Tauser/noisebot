/*
 * attention_service.c — Modelo contínuo de atenção (Layer 5)
 *
 * Decaimento:  level *= exp(-dt_ms / τ),  τ = 30 000 ms
 * Estímulo:    level  = min(1.0, level + weight[src] * intensity)
 *
 * SOUND_LOUD é verificado no tick (cada 500ms) via sound_analysis_get_rms()
 * para sustentar atenção durante fala contínua de alta energia.
 *
 * Thread safety:
 *   s_level é protegido por portMUX spinlock — seção crítica mínima.
 *   Callbacks do event bus rodam na dispatcher task; tick roda na behavior_task.
 */

#include "attention_service.h"

#include "event_bus.h"
#include "nb_events.h"
#include "sound_analysis_service.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_log.h"

#include <math.h>

#define TAG "nb_attn"

/* ── Parâmetros ──────────────────────────────────────────────────────────── */

#define DECAY_TAU_MS       30000.0f  /* τ de decaimento (ms)                  */
#define LOUD_CHECK_PERIOD  500U      /* intervalo de verificação SOUND_LOUD   */
#define LOUD_RMS_THRESHOLD 0.15f     /* RMS mínimo para considerar "alto"     */

/* Pesos por fonte (indexados por nb_attention_source_t) */
static const float k_weight[NB_ATTN_SOURCE_COUNT] = {
    [NB_ATTN_VOICE_START] = 0.70f,
    [NB_ATTN_TOUCH_TAP]   = 0.50f,
    [NB_ATTN_SOUND_CLAP]  = 0.40f,
    [NB_ATTN_SOUND_LOUD]  = 0.20f,
    [NB_ATTN_SOUND_MUSIC] = 0.15f,
};

/* ── Estado interno ──────────────────────────────────────────────────────── */

static float        s_level         = 0.0f;
static portMUX_TYPE s_mux           = portMUX_INITIALIZER_UNLOCKED;
static bool         s_initialized   = false;
static uint32_t     s_loud_check_ms = 0;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void apply_stimulus(nb_attention_source_t src, float intensity)
{
    float delta = k_weight[src] * intensity;
    taskENTER_CRITICAL(&s_mux);
    s_level += delta;
    if (s_level > 1.0f) s_level = 1.0f;
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGD(TAG, "stimulus src=%d δ=%.2f level=%.2f", (int)src, delta, s_level);
}

/* ── Event handlers ──────────────────────────────────────────────────────── */

static void on_voice_start(const nb_event_t *ev, void *ctx)
{
    (void)ev; (void)ctx;
    apply_stimulus(NB_ATTN_VOICE_START, 1.0f);
}

static void on_touch_tap(const nb_event_t *ev, void *ctx)
{
    (void)ev; (void)ctx;
    apply_stimulus(NB_ATTN_TOUCH_TAP, 1.0f);
}

static void on_sound_clap(const nb_event_t *ev, void *ctx)
{
    (void)ev; (void)ctx;
    apply_stimulus(NB_ATTN_SOUND_CLAP, 1.0f);
}

static void on_music_start(const nb_event_t *ev, void *ctx)
{
    (void)ev; (void)ctx;
    apply_stimulus(NB_ATTN_SOUND_MUSIC, 1.0f);
}

/* ── API ─────────────────────────────────────────────────────────────────── */

esp_err_t attention_service_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;

    nb_event_subscribe(NB_EVT_VOICE_ACTIVITY_START, on_voice_start, NULL, NULL);
    nb_event_subscribe(NB_EVT_TOUCH_TAP,            on_touch_tap,   NULL, NULL);
    nb_event_subscribe(NB_EVT_SOUND_CLAP,           on_sound_clap,  NULL, NULL);
    nb_event_subscribe(NB_EVT_SOUND_MUSIC_START,    on_music_start, NULL, NULL);

    s_initialized = true;
    ESP_LOGI(TAG, "attention_service inicializado (τ=%.0fs)", DECAY_TAU_MS / 1000.0f);
    return ESP_OK;
}

void attention_service_on_stimulus(nb_attention_source_t src, float intensity)
{
    if (!s_initialized) return;
    if ((uint32_t)src >= (uint32_t)NB_ATTN_SOURCE_COUNT) return;
    float i = (intensity < 0.0f) ? 0.0f : (intensity > 1.0f ? 1.0f : intensity);
    apply_stimulus(src, i);
}

float attention_service_get_level(void)
{
    taskENTER_CRITICAL(&s_mux);
    float v = s_level;
    taskEXIT_CRITICAL(&s_mux);
    return v;
}

void attention_service_tick(uint32_t dt_ms)
{
    if (!s_initialized) return;

    /* Decaimento exponencial: level *= exp(-dt / τ) */
    float decay = expf(-(float)dt_ms / DECAY_TAU_MS);
    taskENTER_CRITICAL(&s_mux);
    s_level *= decay;
    if (s_level < 0.0f) s_level = 0.0f;
    taskEXIT_CRITICAL(&s_mux);

    /* SOUND_LOUD: sustain durante voz de alta energia (a cada 500ms) */
    s_loud_check_ms += dt_ms;
    if (s_loud_check_ms >= LOUD_CHECK_PERIOD) {
        s_loud_check_ms = 0;
        if (sound_analysis_get_class() == NB_SOUND_VOICE) {
            float rms = sound_analysis_get_rms();
            if (rms > LOUD_RMS_THRESHOLD) {
                apply_stimulus(NB_ATTN_SOUND_LOUD, rms);
            }
        }
    }
}
