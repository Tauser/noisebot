/*
 * rhythm_service.c — Detecção de BPM por autocorrelação (Layer 5)
 *
 * Algoritmo:
 *   1. Amostra RMS a cada tick (100ms) → buffer circular de 64 amostras.
 *   2. Autocorrelação normalizada R[k] / R[0] para lags k = 3..10
 *      (períodos 300–1000ms → 200–60 BPM).
 *   3. Melhor lag = argmax da autocorrelação. BPM = 60000 / (k × 100).
 *   4. LOCKED quando confiança > 0.65; LOST quando < 0.30 por > 2s.
 *   5. BEAT_TICK publicado via phase accumulator (faseado no lock).
 *
 * Nota sobre fala vs música:
 *   Fala tem energia irregular → autocorrelação sem pico claro → NO_LOCK.
 *   Músic tem onset periódico → pico pronunciado → LOCK.
 *   Garantia adicional: só tenta lock quando classe != NB_SOUND_VOICE.
 */

#include "rhythm_service.h"

#include "event_bus.h"
#include "nb_events.h"
#include "sound_analysis_service.h"

#include "freertos/FreeRTOS.h"
#include "esp_log.h"

#include <string.h>
#include <stddef.h>

#define TAG "nb_rhythm"

/* ── Parâmetros ──────────────────────────────────────────────────────────── */

#define RMS_BUF_SIZE     64U     /* amostras @ 100ms = 6.4s de história       */
#define LAG_MIN          3U      /* 300ms → ~200 BPM                          */
#define LAG_MAX          10U     /* 1000ms → 60 BPM                           */
#define LOCK_THRESHOLD   0.65f   /* confiança para LOCKED                     */
#define LOSE_THRESHOLD   0.30f   /* confiança para iniciar contagem de perda  */
#define LOSE_TIME_MS     2000U   /* ms abaixo do threshold antes de LOST      */
#define ACORR_PERIOD_MS  500U    /* intervalo de recálculo da autocorrelação   */
#define MIN_SAMPLES_LOCK (LAG_MAX * 3U) /* mínimo de amostras para tentar lock */

/* ── Estado ──────────────────────────────────────────────────────────────── */

static float    s_buf[RMS_BUF_SIZE]; /* buffer circular de RMS               */
static uint8_t  s_head;              /* próximo índice de escrita             */
static uint8_t  s_filled;            /* amostras válidas (satura em BUF_SIZE) */

static bool     s_locked;
static float    s_bpm;
static float    s_confidence;

static uint32_t s_phase_ms;          /* fase do beat acumulador              */
static uint32_t s_beat_period_ms;    /* período atual em ms                  */

static uint32_t s_acorr_ms;          /* contador para disparo do recálculo   */
static uint32_t s_lose_ms;           /* ms consecutivos abaixo de LOSE_THRESHOLD */

static bool s_initialized;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Lê amostra lógica i (0 = mais antiga) do buffer circular. */
static inline float read_at(uint8_t i)
{
    /* oldest = head - filled; newest = head - 1 */
    return s_buf[(uint8_t)(s_head - s_filled + i)];  /* wrap via uint8 overflow */
}

/*
 * Autocorrelação normalizada R[k] = (Σ x[i]*x[i+k]) / (Σ x[i]^2).
 * Retorna 0 se energia total for negligível.
 */
static float autocorr(uint8_t lag)
{
    if (s_filled <= lag) return 0.0f;

    float r0 = 0.0f;
    float rk = 0.0f;
    uint8_t count = s_filled - lag;

    for (uint8_t i = 0; i < count; i++) {
        float xi = read_at(i);
        r0 += xi * xi;
        rk += xi * read_at((uint8_t)(i + lag));
    }

    if (r0 < 1e-6f) return 0.0f;
    float v = rk / r0;
    return (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/*
 * Acha o lag k em [LAG_MIN, LAG_MAX] com maior autocorrelação.
 * Retorna o lag e escreve o valor em *out_conf.
 */
static uint8_t find_best_lag(float *out_conf)
{
    uint8_t best_k    = LAG_MIN;
    float   best_conf = 0.0f;

    for (uint8_t k = LAG_MIN; k <= LAG_MAX; k++) {
        float c = autocorr(k);
        if (c > best_conf) {
            best_conf = c;
            best_k    = k;
        }
    }

    *out_conf = best_conf;
    return best_k;
}

/* Publica evento simples (sem payload). */
static void publish(nb_event_type_t type)
{
    nb_event_t ev = {
        .type = type,
        .timestamp_ms = 0,
        .data.u32 = 0,
    };
    nb_event_publish(&ev);
}

/* ── API ─────────────────────────────────────────────────────────────────── */

esp_err_t rhythm_service_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;

    memset(s_buf, 0, sizeof(s_buf));
    s_head           = 0;
    s_filled         = 0;
    s_locked         = false;
    s_bpm            = 0.0f;
    s_confidence     = 0.0f;
    s_phase_ms       = 0;
    s_beat_period_ms = 500;  /* placeholder, sobrescrito ao lock */
    s_acorr_ms       = 0;
    s_lose_ms        = 0;
    s_initialized    = true;

    ESP_LOGI(TAG, "rhythm_service inicializado (buf=%u, lag %u..%u)", RMS_BUF_SIZE, LAG_MIN, LAG_MAX);
    return ESP_OK;
}

void rhythm_service_tick(uint32_t dt_ms)
{
    if (!s_initialized) return;

    /* 1. Amostrar RMS — zeros durante silêncio e voz (evita lock em fala) */
    nb_sound_class_t cls = sound_analysis_get_class();
    float rms = (cls == NB_SOUND_SILENCE || cls == NB_SOUND_VOICE)
                ? 0.0f
                : sound_analysis_get_rms();

    s_buf[s_head] = rms;
    s_head = (s_head + 1) % RMS_BUF_SIZE;
    if (s_filled < RMS_BUF_SIZE) s_filled++;

    /* 2. Beat tick quando locked */
    if (s_locked && s_beat_period_ms > 0) {
        s_phase_ms += dt_ms;
        if (s_phase_ms >= s_beat_period_ms) {
            s_phase_ms -= s_beat_period_ms;
            publish(NB_EVT_BEAT_TICK);
        }
    }

    /* 3. Recalcular autocorrelação a cada ACORR_PERIOD_MS */
    s_acorr_ms += dt_ms;
    if (s_acorr_ms < ACORR_PERIOD_MS) return;
    s_acorr_ms = 0;

    if (s_filled < MIN_SAMPLES_LOCK) return;

    float conf;
    uint8_t best_k = find_best_lag(&conf);
    s_confidence   = conf;

    if (!s_locked) {
        if (conf >= LOCK_THRESHOLD) {
            s_locked         = true;
            s_beat_period_ms = (uint32_t)best_k * 100U;
            s_bpm            = 60000.0f / (float)s_beat_period_ms;
            s_phase_ms       = 0;
            s_lose_ms        = 0;
            ESP_LOGI(TAG, "RHYTHM_LOCKED bpm=%.1f conf=%.2f (k=%u)", s_bpm, conf, (unsigned)best_k);
            publish(NB_EVT_RHYTHM_LOCKED);
        }
    } else {
        /* Atualiza BPM contínuo */
        s_beat_period_ms = (uint32_t)best_k * 100U;
        s_bpm            = 60000.0f / (float)s_beat_period_ms;

        if (conf < LOSE_THRESHOLD) {
            s_lose_ms += ACORR_PERIOD_MS;
            if (s_lose_ms >= LOSE_TIME_MS) {
                s_locked     = false;
                s_bpm        = 0.0f;
                s_lose_ms    = 0;
                ESP_LOGI(TAG, "RHYTHM_LOST (conf=%.2f)", conf);
                publish(NB_EVT_RHYTHM_LOST);
            }
        } else {
            s_lose_ms = 0;
        }
    }
}

float rhythm_service_get_bpm(void)
{
    return s_bpm;
}

float rhythm_service_get_confidence(void)
{
    return s_confidence;
}

bool rhythm_service_is_locked(void)
{
    return s_locked;
}
