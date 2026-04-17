/*
 * synth_service.c — Síntese procedural de áudio (Layer 4)
 *
 * Arquitetura de sample generation:
 *   - Um único "voice" ativo por vez (sem polifonia).
 *   - Osciladores: sine, square, sawtooth via phase accumulator.
 *   - Ruído: xorshift32 + IIR LP filter + LFO de amplitude.
 *   - Envelope ADSR: calculado por sample index (sem estado de stage).
 *   - Chirp: frequência interpolada linearmente por sample.
 *   - Melody: notas sequenciais; quando nota termina, inicia a próxima
 *     dentro do mesmo fill_chunk (sem lacuna de 16ms entre notas).
 *
 * Thread safety:
 *   Primitivos (caller task) e synth_fill_chunk (audio_task) compartilham
 *   o estado do voice via FreeRTOS mutex.
 */

#include "synth_service.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_random.h"
#include "esp_log.h"
#include "logger.h"

#include <math.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define TAG "nb_synth"

#define SAMPLE_RATE  16000U

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* ── Voice state ─────────────────────────────────────────────────────────── */

static SemaphoreHandle_t s_mutex;

static struct {
    bool initialized;

    /* Volume: 0–100, aplica o mesmo escalonamento que o audio_service usa no WAV */
    uint8_t volume;

    /* Default timbre (changed by synth_set_timbre) */
    nb_synth_timbre_t timbre_default;

    /* Active voice */
    bool active;
    nb_synth_timbre_t timbre;

    /* Oscillator */
    float phase;        /* current oscillator phase, 0 .. 2π             */
    float freq;         /* instantaneous frequency at sample 0            */
    float freq_step;    /* frequency delta per sample (chirp slope)       */

    /* ADSR parameters (in samples) */
    uint32_t att_samp;
    uint32_t dec_samp;
    float    sus_level;
    uint32_t rel_samp;
    uint32_t total_samp;
    uint32_t done_samp;

    /* Noise state */
    uint32_t lfsr;           /* xorshift32 state                           */
    float    noise_lp;       /* IIR lowpass state                          */
    float    noise_lfo_ph;   /* LFO phase 0..2π                            */
    float    noise_intensity;

    /* Melody: internal copy of note list */
    nb_note_t mel_buf[SYNTH_MAX_MELODY_NOTES];
    uint8_t   mel_count;
    uint8_t   mel_idx;
    bool      is_melody;
} s;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Uniform jitter: returns base * (1 ± frac).  frac = 0.08 → ±8%. */
static float rand_jitter(float base, float frac)
{
    float r = (float)(esp_random() & 0xFFFFU) / 65535.0f; /* 0..1 */
    return base * (1.0f + frac * (2.0f * r - 1.0f));
}

static uint32_t rand_jitter_ms(uint32_t base_ms, float frac)
{
    float v = rand_jitter((float)base_ms, frac);
    if (v < 1.0f) v = 1.0f;
    return (uint32_t)v;
}

/* Converts milliseconds to samples at SAMPLE_RATE. */
static uint32_t ms_to_samp(float ms)
{
    float s_count = ms * (float)SAMPLE_RATE / 1000.0f;
    if (s_count < 0.0f) return 0;
    return (uint32_t)s_count;
}

/* ── Voice setup (call with mutex held) ──────────────────────────────────── */

static void voice_start(nb_synth_timbre_t timbre,
                        float freq_start, float freq_end,
                        uint32_t duration_ms,
                        float att_ms, float dec_ms,
                        float sus_level, float rel_ms,
                        float intensity)
{
    s.active     = false;   /* disable while setting up */
    s.timbre     = timbre;
    s.phase      = 0.0f;
    s.freq       = freq_start;
    s.is_melody  = false;

    uint32_t total = ms_to_samp((float)duration_ms);
    if (total == 0) total = 1;
    s.total_samp  = total;
    s.done_samp   = 0;
    s.freq_step   = (freq_end - freq_start) / (float)total;

    s.att_samp  = ms_to_samp(att_ms);
    s.dec_samp  = ms_to_samp(dec_ms);
    s.sus_level = sus_level;
    s.rel_samp  = ms_to_samp(rel_ms);

    /* Clamp so ADSR fits within total */
    uint32_t env_used = s.att_samp + s.dec_samp + s.rel_samp;
    if (env_used > s.total_samp) {
        /* Scale down proportionally */
        float scale = (float)s.total_samp / (float)env_used;
        s.att_samp = (uint32_t)((float)s.att_samp * scale);
        s.dec_samp = (uint32_t)((float)s.dec_samp * scale);
        s.rel_samp = s.total_samp - s.att_samp - s.dec_samp;
    }

    s.noise_lp        = 0.0f;
    s.noise_lfo_ph    = 0.0f;
    s.noise_intensity = intensity;

    s.active = true;
}

/* Start one note of a melody (call with mutex held). */
static void voice_start_note(const nb_note_t *note)
{
    /* Silence/pause: low-amplitude sine at inaudible freq for duration */
    float freq = (note->freq > 0.0f) ? note->freq : 20.0f;
    float amp  = (note->freq > 0.0f) ? 1.0f : 0.0f;

    float att_ms = rand_jitter(8.0f, 0.2f);
    float rel_ms = rand_jitter(15.0f, 0.2f);

    voice_start(s.timbre_default, freq, freq,
                note->duration_ms,
                att_ms, 0.0f, amp, rel_ms,
                1.0f);
}

/* ── Sample generation (call with mutex held) ────────────────────────────── */

/* ADSR envelope value at absolute sample index i. */
static float envelope_at(uint32_t i)
{
    if (i >= s.total_samp) return 0.0f;

    /* Attack */
    if (s.att_samp > 0 && i < s.att_samp) {
        return (float)i / (float)s.att_samp;
    }

    /* Decay */
    uint32_t i_dec = i - s.att_samp;
    if (s.dec_samp > 0 && i_dec < s.dec_samp) {
        float t = (float)i_dec / (float)s.dec_samp;
        return 1.0f - t * (1.0f - s.sus_level);
    }

    /* Release (from end) */
    if (s.rel_samp > 0 && i >= s.total_samp - s.rel_samp) {
        uint32_t i_rel = i - (s.total_samp - s.rel_samp);
        float t = (float)i_rel / (float)s.rel_samp;
        if (t > 1.0f) t = 1.0f;
        return s.sus_level * (1.0f - t);
    }

    return s.sus_level;
}

/*
 * Generate one sample.  Advances oscillator phase and noise state.
 * i_abs = absolute sample index since voice start.
 */
static float generate_sample(uint32_t i_abs)
{
    float freq_i = s.freq + s.freq_step * (float)i_abs;

    if (s.timbre == NB_SYNTH_TIMBRE_NOISE) {
        /* xorshift32 noise */
        s.lfsr ^= s.lfsr << 13;
        s.lfsr ^= s.lfsr >> 17;
        s.lfsr ^= s.lfsr << 5;
        float raw = (float)(int32_t)s.lfsr * (1.0f / 2147483648.0f);

        /* IIR lowpass ~300 Hz: y = 0.12*x + 0.88*y_prev */
        s.noise_lp = 0.12f * raw + 0.88f * s.noise_lp;

        /* LFO amplitude modulation at ~5 Hz */
        float lfo = 0.7f + 0.3f * sinf(s.noise_lfo_ph);
        s.noise_lfo_ph += 2.0f * (float)M_PI * 5.0f / (float)SAMPLE_RATE;
        if (s.noise_lfo_ph >= 2.0f * (float)M_PI) {
            s.noise_lfo_ph -= 2.0f * (float)M_PI;
        }
        return s.noise_lp * lfo * s.noise_intensity;
    }

    float sample;
    switch (s.timbre) {
    case NB_SYNTH_TIMBRE_SQUARE:
        sample = (s.phase < (float)M_PI) ? 1.0f : -1.0f;
        break;
    case NB_SYNTH_TIMBRE_SAW:
        /* Ramp from -1 at phase=0 to +1 at phase=2π, then jump */
        sample = s.phase / (float)M_PI - 1.0f;
        break;
    case NB_SYNTH_TIMBRE_SINE:
    default:
        sample = sinf(s.phase);
        break;
    }

    /* Advance phase */
    s.phase += 2.0f * (float)M_PI * freq_i / (float)SAMPLE_RATE;
    while (s.phase >= 2.0f * (float)M_PI) {
        s.phase -= 2.0f * (float)M_PI;
    }
    return sample;
}

/* ── API ─────────────────────────────────────────────────────────────────── */

esp_err_t synth_init(void)
{
    if (s.initialized) return ESP_ERR_INVALID_STATE;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        NB_LOGE(TAG, "mutex create falhou");
        return ESP_ERR_NO_MEM;
    }

    s.timbre_default = NB_SYNTH_TIMBRE_SINE;
    s.volume         = 80U;
    s.lfsr           = esp_random();
    if (s.lfsr == 0) s.lfsr = 0xDEADBEEFU; /* xorshift requires non-zero state */
    s.initialized    = true;

    NB_LOGI(TAG, "synth_service init (ADSR, 4 timbres, melody max %u notas)",
            (unsigned)SYNTH_MAX_MELODY_NOTES);
    return ESP_OK;
}

bool synth_fill_chunk(int16_t *out_buf, size_t samples)
{
    if (!s.initialized) return false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (!s.active) {
        xSemaphoreGive(s_mutex);
        return false;
    }

    for (size_t i = 0; i < samples; ) {
        if (s.done_samp >= s.total_samp) {
            /* Current note/voice ended */
            if (s.is_melody && s.mel_idx < s.mel_count) {
                /* Start next note, continue filling same buffer */
                voice_start_note(&s.mel_buf[s.mel_idx++]);
            } else {
                s.active = false;
                /* Zero-fill remaining samples */
                memset(out_buf + i, 0, (samples - i) * sizeof(int16_t));
                break;
            }
        }

        float env    = envelope_at(s.done_samp);
        float sample = generate_sample(s.done_samp) * env;
        s.done_samp++;

        /* Scale to int16 with volume (0-100) applied */
        int32_t v = (int32_t)(sample * 0.85f * 32767.0f);
        v = (v * (int32_t)(((uint32_t)s.volume * 256U) / 100U)) >> 8;
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        out_buf[i++] = (int16_t)v;
    }

    xSemaphoreGive(s_mutex);
    return true;
}

void synth_chirp(float freq_start, float freq_end, uint32_t duration_ms)
{
    if (!s.initialized) return;

    float fs  = rand_jitter(freq_start, 0.08f);
    float fe  = rand_jitter(freq_end,   0.08f);
    uint32_t d = rand_jitter_ms(duration_ms, 0.10f);

    float att_ms = rand_jitter(8.0f,  0.20f);
    float rel_ms = rand_jitter(20.0f, 0.20f);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    voice_start(s.timbre_default, fs, fe, d,
                att_ms, 0.0f, 1.0f, rel_ms, 1.0f);
    xSemaphoreGive(s_mutex);
}

void synth_purr(uint32_t duration_ms, float intensity)
{
    if (!s.initialized) return;

    uint32_t d = rand_jitter_ms(duration_ms, 0.05f);
    float    it = intensity < 0.0f ? 0.0f : (intensity > 1.0f ? 1.0f : intensity);
    it = rand_jitter(it, 0.10f);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* Noise with slow attack and slow release */
    voice_start(NB_SYNTH_TIMBRE_NOISE, 0.0f, 0.0f, d,
                rand_jitter(200.0f, 0.20f),  /* att_ms */
                0.0f,                         /* dec_ms */
                1.0f,                         /* sus_level */
                rand_jitter(300.0f, 0.20f),  /* rel_ms */
                it);
    xSemaphoreGive(s_mutex);
}

void synth_blip(float freq, uint32_t duration_ms)
{
    if (!s.initialized) return;

    float    f = rand_jitter(freq, 0.08f);
    uint32_t d = rand_jitter_ms(duration_ms, 0.10f);

    float att_ms = rand_jitter(5.0f,  0.25f);
    float rel_ms = rand_jitter((float)d * 0.4f, 0.20f); /* 40% of duration */

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    voice_start(s.timbre_default, f, f, d,
                att_ms, 0.0f, 0.9f, rel_ms, 1.0f);
    xSemaphoreGive(s_mutex);
}

void synth_melody(const nb_note_t *notes, uint8_t count)
{
    if (!s.initialized || !notes || count == 0) return;

    uint8_t n = (count > SYNTH_MAX_MELODY_NOTES) ? (uint8_t)SYNTH_MAX_MELODY_NOTES : count;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(s.mel_buf, notes, n * sizeof(nb_note_t));
    s.mel_count  = n;
    s.mel_idx    = 0;
    s.is_melody  = true;
    /* Start first note */
    voice_start_note(&s.mel_buf[s.mel_idx++]);
    xSemaphoreGive(s_mutex);
}

void synth_set_timbre(nb_synth_timbre_t t)
{
    if (!s.initialized) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s.timbre_default = t;
    xSemaphoreGive(s_mutex);
}

void synth_play_for_emotion(nb_synth_emotion_hint_t hint)
{
    if (!s.initialized) return;

    switch (hint) {
    case NB_SYNTH_HAPPY:
        /* Chirp ascendente duplo: alegre e energético */
        synth_set_timbre(NB_SYNTH_TIMBRE_SINE);
        synth_chirp(rand_jitter(600.0f, 0.10f),
                    rand_jitter(1200.0f, 0.10f),
                    rand_jitter_ms(160, 0.15f));
        break;

    case NB_SYNTH_CURIOUS:
        /* Chirp curto ascendente + leve inflexão */
        synth_set_timbre(NB_SYNTH_TIMBRE_SINE);
        synth_chirp(rand_jitter(500.0f, 0.10f),
                    rand_jitter(900.0f, 0.10f),
                    rand_jitter_ms(200, 0.12f));
        break;

    case NB_SYNTH_SAD:
        /* Descendente lento, freq baixa, tom suave */
        synth_set_timbre(NB_SYNTH_TIMBRE_SINE);
        synth_chirp(rand_jitter(380.0f, 0.10f),
                    rand_jitter(200.0f, 0.10f),
                    rand_jitter_ms(450, 0.15f));
        break;

    case NB_SYNTH_ALARMED: {
        /* Burst rápido: square alto + chirp agudo */
        synth_set_timbre(NB_SYNTH_TIMBRE_SQUARE);
        synth_chirp(rand_jitter(1400.0f, 0.15f),
                    rand_jitter(2200.0f, 0.15f),
                    rand_jitter_ms(90, 0.20f));
        break;
    }

    case NB_SYNTH_SLEEPY:
        /* Sinusoide grave com longo fade */
        synth_set_timbre(NB_SYNTH_TIMBRE_SINE);
        synth_blip(rand_jitter(140.0f, 0.15f),
                   rand_jitter_ms(700, 0.15f));
        break;

    case NB_SYNTH_SURPRISED:
        /* Chirp muito rápido, grande sweep */
        synth_set_timbre(NB_SYNTH_TIMBRE_SINE);
        synth_chirp(rand_jitter(250.0f, 0.10f),
                    rand_jitter(1600.0f, 0.10f),
                    rand_jitter_ms(110, 0.20f));
        break;

    case NB_SYNTH_FOCUSED:
    case NB_SYNTH_SUSPICIOUS:
        /* Tom médio curto e limpo */
        synth_set_timbre(NB_SYNTH_TIMBRE_SINE);
        synth_blip(rand_jitter(480.0f, 0.10f),
                   rand_jitter_ms(120, 0.12f));
        break;

    case NB_SYNTH_NEUTRAL:
    default:
        /* Blip neutro grave */
        synth_set_timbre(NB_SYNTH_TIMBRE_SINE);
        synth_blip(rand_jitter(350.0f, 0.08f),
                   rand_jitter_ms(100, 0.10f));
        break;
    }
}

void synth_stop(void)
{
    if (!s.initialized) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s.active = false;
    xSemaphoreGive(s_mutex);
}

void synth_set_volume(uint8_t level)
{
    if (!s.initialized) return;
    if (level > 100U) level = 100U;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s.volume = level;
    xSemaphoreGive(s_mutex);
}

bool synth_is_active(void)
{
    return s.active;
}
