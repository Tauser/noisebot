/*
 * sound_analysis_service.c — Análise de áudio em tempo real (Layer 4)
 *
 * Implementação:
 *   - FFT radix-2 DIT de 256 pontos (float, sem biblioteca externa).
 *   - Janela de Hann pré-computada para reduzir spectral leakage.
 *   - Bins: 16000/256 = 62.5 Hz/bin; úteis: 1–127 (62.5Hz–7937.5Hz).
 *   - Classificação por energia de faixa + detecção de transitório (clap).
 *
 * Thresholds marcados TUNEABLE — ajustar com base em medições no hardware.
 */

#include "sound_analysis_service.h"

#include "event_bus.h"
#include "nb_events.h"
#include "logger.h"

#include "esp_timer.h"
#include "esp_log.h"

#include <math.h>
#include <string.h>
#include <stdbool.h>

#define TAG "nb_sa"

/* ── FFT ─────────────────────────────────────────────────────────────────── */

#define FFT_N       256U
#define FFT_HALF    (FFT_N / 2U)

/* s_mic_buf values are 24-bit (audio_hal already did >>8 from raw DMA).
 * >>8 again maps to int16 range. INT16_SCALE normalizes to ±1.0. */
#define INT16_SCALE (1.0f / 32768.0f)

/* Trig constant */
#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* Static processing buffers — kept out of stack */
static float s_fft_re[FFT_N];   /* real part (input + in-place FFT) */
static float s_fft_im[FFT_N];   /* imaginary part                    */
static float s_mag   [FFT_HALF]; /* |X[k]|^2 for k = 0..127          */
static float s_window[FFT_N];   /* Hann window coefficients           */

/* ── Band bin indices (bin k → frequency k × 62.5 Hz) ───────────────────── */

#define BIN_LO_RUMBLE   1U   /* 62.5 Hz */
#define BIN_HI_RUMBLE   4U   /* 250 Hz  */
#define BIN_LO_VOICE    5U   /* 312 Hz  */
#define BIN_HI_VOICE   54U   /* 3375 Hz */
#define BIN_HI_VOICE_LOW 11U /* 687 Hz  */
#define BIN_LO_VOICE_MID 12U /* 750 Hz  */
#define BIN_LO_WHISTLE 16U   /* 1000 Hz */
#define BIN_HI_WHISTLE 48U   /* 3000 Hz */
#define BIN_LO_HIGH    54U   /* 3375 Hz */
#define BIN_HI_HIGH   100U   /* 6250 Hz */

/* ── Thresholds (TUNEABLE) ───────────────────────────────────────────────── */

/* Silence: normalized RMS below this → class SILENCE.
 * Depois do high-pass no audio_service, o piso útil cai bastante. */
#define RMS_SILENCE      0.006f    /* ≈ int16 value ~197 */

/* Clap: broadband transient */
#define CLAP_RMS_MIN     0.04f     /* minimum RMS for clap candidate */
#define CLAP_BROAD_FRAC  0.20f     /* E_high / E_total must exceed this */
#define CLAP_ONSET_RATIO 2.5f      /* current RMS / prev RMS for fast onset */
#define CLAP_WINDOW_CH   25U       /* 400ms history for second-clap detection */
#define CLAP_MIN_GAP_CH  5U        /* 80ms minimum between two clap peaks */

/* Whistle: narrowband 1-3kHz, sustained */
#define WHISTLE_RMS_MIN     0.018f /* minimum RMS for whistle candidate */
#define WHISTLE_BAND_FRAC   0.35f  /* E_whistle / E_total threshold */
#define WHISTLE_ONSET_CH   19U     /* 300ms = 19 chunks to declare WHISTLE */
#define WHISTLE_OFFSET_CH   6U     /* 100ms of non-whistle before reset */

/* Voice: spectral energy in voice band */
#define VOICE_RMS_MIN    0.010f
#define VOICE_BAND_FRAC  0.48f     /* E_voice / E_total threshold */

/* Music: sustained broadband above silence */
#define MUSIC_RMS_MIN    0.020f
#define MUSIC_BROAD_FRAC 0.14f     /* E_high / E_total threshold */
#define MUSIC_ONSET_CH   188U      /* 3s = 188 chunks */
#define MUSIC_OFFSET_CH  125U      /* 2s = 125 chunks */

/* Class debounce: require N identical frames before emitting CLASS_CHANGED */
#define CLASS_DEBOUNCE 2U

/* ── Module state ────────────────────────────────────────────────────────── */

static struct {
    bool initialized;

    /* Public query state — written by audio_task, read by any task */
    volatile nb_sound_class_t class_cur;
    volatile float rms;
    volatile float dominant_freq;
    volatile float voice_ratio;
    volatile float voice_low_ratio;
    volatile float voice_mid_ratio;
    volatile float high_ratio;
    volatile float low_ratio;

    /* Class debounce */
    nb_sound_class_t class_pending;
    uint8_t          class_pending_cnt;

    /* Clap: circular RMS history + peak tracking */
    float   clap_hist[CLAP_WINDOW_CH];
    uint8_t clap_hist_head;
    uint8_t clap_peak_age;  /* chunks since last clap peak (saturates at 255) */

    /* Whistle: consecutive signature chunks */
    uint16_t whistle_on_cnt;
    uint16_t whistle_off_cnt;

    /* Music: onset/offset counters + active flag */
    uint16_t music_on_cnt;
    uint16_t music_off_cnt;
    bool     music_active;
} s;

/* ── FFT: radix-2 Cooley-Tukey DIT ──────────────────────────────────────── */

/*
 * Bit-reversal permutation in-place on paired (re, im) arrays.
 * Standard algorithm for N = power of 2.
 */
static void fft_bitrev(float *re, float *im)
{
    for (unsigned i = 1, j = 0; i < FFT_N; i++) {
        unsigned bit = FFT_N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t;
            t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
}

/*
 * 256-point radix-2 DIT FFT.
 *
 * Input : re[k] = windowed PCM sample k, im[k] = 0.
 * Output: re[k], im[k] = complex spectrum X[k], k = 0..255.
 *
 * Twiddle factors are computed inline (8 cos/sin calls per FFT = ~16µs).
 * No twiddle table needed — keeps SRAM usage minimal.
 */
static void fft256(float *re, float *im)
{
    fft_bitrev(re, im);

    for (unsigned len = 2; len <= FFT_N; len <<= 1) {
        /* Base twiddle for this stage: W_len^1 = e^{-j2π/len} */
        float ang = -(float)M_PI / (float)(len >> 1);
        float wRe = cosf(ang);
        float wIm = sinf(ang);

        for (unsigned i = 0; i < FFT_N; i += len) {
            float uRe = 1.0f, uIm = 0.0f;   /* W_len^j, start at j=0 */
            unsigned half = len >> 1;

            for (unsigned j = 0; j < half; j++) {
                unsigned a = i + j;
                unsigned b = a + half;

                /* Butterfly: X[a] ± W * X[b] */
                float tre = uRe * re[b] - uIm * im[b];
                float tim = uRe * im[b] + uIm * re[b];
                re[b] = re[a] - tre;
                im[b] = im[a] - tim;
                re[a] = re[a] + tre;
                im[a] = im[a] + tim;

                /* Advance twiddle factor by one step */
                float nRe = uRe * wRe - uIm * wIm;
                uIm       = uRe * wIm + uIm * wRe;
                uRe       = nRe;
            }
        }
    }
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Sum of s_mag[k] for lo <= k <= hi, clamped to FFT_HALF. */
static float band_energy(unsigned lo, unsigned hi)
{
    float e = 0.0f;
    for (unsigned k = lo; k <= hi && k < FFT_HALF; k++) {
        e += s_mag[k];
    }
    return e;
}

/* Publish async event — called from audio_task context (safe, not dispatcher). */
static void publish(nb_event_type_t type, uint32_t payload)
{
    nb_event_t evt = {
        .type         = type,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000LL),
        .data.u32     = payload,
    };
    nb_event_publish_async(&evt);
}

/* ── API ─────────────────────────────────────────────────────────────────── */

esp_err_t sound_analysis_init(void)
{
    if (s.initialized) return ESP_ERR_INVALID_STATE;

    /* Pre-compute Hann window: w[n] = 0.5 * (1 - cos(2π*n/(N-1))) */
    for (unsigned i = 0; i < FFT_N; i++) {
        s_window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI
                      * (float)i / (float)(FFT_N - 1U)));
    }

    /* Zero state (initialized = false already since BSS) — set fields explicitly */
    s.class_cur          = NB_SOUND_SILENCE;
    s.class_pending      = NB_SOUND_SILENCE;
    s.clap_peak_age      = (uint8_t)CLAP_WINDOW_CH; /* "no recent peak" */
    s.initialized        = true;

    NB_LOGI(TAG, "sound_analysis init (FFT %u pts, %.1f Hz/bin)",
            FFT_N, (float)16000U / (float)FFT_N);
    return ESP_OK;
}

void sound_analysis_tick(const int16_t *pcm, size_t samples)
{
    if (!s.initialized || !pcm || samples == 0) return;

    size_t n = (samples > FFT_N) ? FFT_N : samples;

    /* ── 1. Normalize to float, compute RMS (pre-window), apply Hann ──── */
    float rms_sq = 0.0f;
    for (size_t i = 0; i < n; i++) {
        float x = (float)pcm[i] * INT16_SCALE;
        rms_sq       += x * x;
        s_fft_re[i]   = x * s_window[i];
        s_fft_im[i]   = 0.0f;
    }
    /* Zero-pad if chunk was shorter than 256 */
    for (size_t i = n; i < FFT_N; i++) {
        s_fft_re[i] = 0.0f;
        s_fft_im[i] = 0.0f;
    }
    float rms_cur = sqrtf(rms_sq / (float)n);

    /* ── 2. FFT ──────────────────────────────────────────────────────── */
    fft256(s_fft_re, s_fft_im);

    /* ── 3. Magnitude spectrum (power) + dominant bin ────────────────── */
    float    e_total  = 0.0f;
    float    dom_mag  = 0.0f;
    unsigned dom_bin  = 1U;

    s_mag[0] = 0.0f; /* Discard DC */
    for (unsigned k = 1U; k < FFT_HALF; k++) {
        float mag    = s_fft_re[k] * s_fft_re[k] + s_fft_im[k] * s_fft_im[k];
        s_mag[k]     = mag;
        e_total     += mag;
        if (mag > dom_mag) { dom_mag = mag; dom_bin = k; }
    }

    float dom_freq = (dom_mag > 0.0f)
                   ? ((float)dom_bin * 16000.0f / (float)FFT_N)
                   : 0.0f;

    /* ── 4. Band energy fractions ────────────────────────────────────── */
    float e_voice     = band_energy(BIN_LO_VOICE,     BIN_HI_VOICE);
    float e_voice_low = band_energy(BIN_LO_VOICE,     BIN_HI_VOICE_LOW);
    float e_voice_mid = band_energy(BIN_LO_VOICE_MID, BIN_HI_VOICE);
    float e_low       = band_energy(BIN_LO_RUMBLE,    BIN_HI_RUMBLE);
    float e_whistle   = band_energy(BIN_LO_WHISTLE,   BIN_HI_WHISTLE);
    float e_high      = band_energy(BIN_LO_HIGH,      BIN_HI_HIGH);
    float e_frac_v    = (e_total > 0.0f) ? (e_voice     / e_total) : 0.0f;
    float e_frac_vl   = (e_total > 0.0f) ? (e_voice_low / e_total) : 0.0f;
    float e_frac_vm   = (e_total > 0.0f) ? (e_voice_mid / e_total) : 0.0f;
    float e_frac_l    = (e_total > 0.0f) ? (e_low       / e_total) : 0.0f;
    float e_frac_w    = (e_total > 0.0f) ? (e_whistle   / e_total) : 0.0f;
    float e_frac_h    = (e_total > 0.0f) ? (e_high      / e_total) : 0.0f;

    /* ── 5. Update public query fields ───────────────────────────────── */
    s.rms           = rms_cur;
    s.dominant_freq = dom_freq;
    s.voice_ratio   = e_frac_v;
    s.voice_low_ratio = e_frac_vl;
    s.voice_mid_ratio = e_frac_vm;
    s.high_ratio    = e_frac_h;
    s.low_ratio     = e_frac_l;

    /* ── 6. Instantaneous classification ────────────────────────────── */

    nb_sound_class_t inst = NB_SOUND_SILENCE;

    if (rms_cur < RMS_SILENCE) {
        inst = NB_SOUND_SILENCE;
    } else {
        /* ── Clap detection (transient broadband peak) ────────────────
         *
         * Push RMS into circular history.  A "peak" is:
         *   - RMS above CLAP_RMS_MIN
         *   - broadband ratio above CLAP_BROAD_FRAC
         *   - fast onset: current RMS > CLAP_ONSET_RATIO × previous
         *
         * Emit NB_EVT_SOUND_CLAP on the SECOND peak within 400ms window.
         */
        float prev_rms = s.clap_hist[(s.clap_hist_head + CLAP_WINDOW_CH - 1U)
                                     % CLAP_WINDOW_CH];
        s.clap_hist[s.clap_hist_head] = rms_cur;
        s.clap_hist_head = (uint8_t)((s.clap_hist_head + 1U) % CLAP_WINDOW_CH);

        if (s.clap_peak_age < 255U) s.clap_peak_age++;

        bool is_peak = (rms_cur > CLAP_RMS_MIN)
                    && (e_frac_h > CLAP_BROAD_FRAC)
                    && (prev_rms < RMS_SILENCE || rms_cur > prev_rms * CLAP_ONSET_RATIO);

        if (is_peak && s.clap_peak_age > CLAP_MIN_GAP_CH) {
            if (s.clap_peak_age <= CLAP_WINDOW_CH) {
                /* Second peak within window → clap pair detected */
                publish(NB_EVT_SOUND_CLAP, 0U);
                NB_LOGI(TAG, "CLAP detectado (RMS=%.4f)", rms_cur);
            }
            s.clap_peak_age = 0U;
        }

        /* Clap is a point event; during and after it the class is NOISE */
        if (rms_cur >= WHISTLE_RMS_MIN && e_frac_w > WHISTLE_BAND_FRAC) {
            inst = NB_SOUND_WHISTLE;
        } else if (rms_cur >= VOICE_RMS_MIN && e_frac_v > VOICE_BAND_FRAC) {
            inst = NB_SOUND_VOICE;
        } else if (rms_cur >= MUSIC_RMS_MIN && e_frac_h > MUSIC_BROAD_FRAC) {
            inst = NB_SOUND_MUSIC;
        } else {
            inst = NB_SOUND_NOISE;
        }
    }

    /* ── 7. Whistle: sustained narrowband ────────────────────────────── */
    if (inst == NB_SOUND_WHISTLE) {
        s.whistle_off_cnt = 0U;
        s.whistle_on_cnt++;
        if (s.whistle_on_cnt == WHISTLE_ONSET_CH) {
            publish(NB_EVT_SOUND_WHISTLE, 0U);
            NB_LOGI(TAG, "WHISTLE detectado (f=%.0fHz)", dom_freq);
        }
    } else {
        s.whistle_off_cnt++;
        if (s.whistle_off_cnt >= WHISTLE_OFFSET_CH) {
            s.whistle_on_cnt = 0U;
        }
    }

    /* ── 8. Music: sustained broadband ──────────────────────────────── */
    if (inst == NB_SOUND_MUSIC) {
        s.music_off_cnt = 0U;
        s.music_on_cnt++;
        if (!s.music_active && s.music_on_cnt >= MUSIC_ONSET_CH) {
            s.music_active = true;
            publish(NB_EVT_SOUND_MUSIC_START, 0U);
            NB_LOGI(TAG, "MUSIC_START detectado");
        }
    } else {
        if (s.music_on_cnt > 0U) s.music_on_cnt = 0U;
        s.music_off_cnt++;
        if (s.music_active && s.music_off_cnt >= MUSIC_OFFSET_CH) {
            s.music_active = false;
            publish(NB_EVT_SOUND_MUSIC_END, 0U);
            NB_LOGI(TAG, "MUSIC_END detectado");
        }
    }

    /* ── 9. Class debounce → NB_EVT_SOUND_CLASS_CHANGED ─────────────── */
    if (inst == s.class_pending) {
        if (s.class_pending_cnt < 255U) s.class_pending_cnt++;
    } else {
        s.class_pending     = inst;
        s.class_pending_cnt = 1U;
    }

    if (s.class_pending_cnt >= CLASS_DEBOUNCE && inst != s.class_cur) {
        s.class_cur = inst;
        publish(NB_EVT_SOUND_CLASS_CHANGED, (uint32_t)inst);
    }
}

nb_sound_class_t sound_analysis_get_class(void)
{
    return s.class_cur;
}

float sound_analysis_get_rms(void)
{
    return s.rms;
}

float sound_analysis_get_voice_ratio(void)
{
    return s.voice_ratio;
}

float sound_analysis_get_voice_low_ratio(void)
{
    return s.voice_low_ratio;
}

float sound_analysis_get_voice_mid_ratio(void)
{
    return s.voice_mid_ratio;
}

float sound_analysis_get_high_ratio(void)
{
    return s.high_ratio;
}

float sound_analysis_get_low_ratio(void)
{
    return s.low_ratio;
}

float sound_analysis_get_dominant_freq(void)
{
    return s.dominant_freq;
}
