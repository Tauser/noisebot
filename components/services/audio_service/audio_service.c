/*
 * audio_service.c — Serviço de áudio do NoiseBot (Layer 4)
 *
 * Não depende de infra (sem event_bus, sem config_manager) para evitar
 * dependência circular. Eventos são emitidos via callback (nb_audio_event_cb_t)
 * registrado pelo boot_manager, que faz a ponte para o event bus.
 *
 * Loop da task (~16ms por iteração):
 *   1. Preparar e escrever chunk TX (áudio WAV ou silêncio) — manter DMA alimentado.
 *   2. Ler chunk RX do microfone.
 *   3. Atualizar VAD: RMS sobre janela de NB_AUDIO_CHUNK_FRAMES amostras.
 *   4. Gravação de diagnóstico: escrever amostras no arquivo WAV se ativo.
 */

#include "audio_service.h"
#include "audio_hal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#define TAG "audio_svc"

/* Mínimo de chunks consecutivos acima do threshold para declarar VAD START (5 × 16ms = 80ms). */
#define VAD_ENTER_CHUNKS 5U

/* Log de ruído ambiente a cada N chunks quando em silêncio (~2s). */
#define VAD_NOISE_LOG_CHUNKS 125U

/* ── Configuração da task ────────────────────────────────────────────────── */

#define AUDIO_TASK_STACK    4096U
#define AUDIO_TASK_PRIORITY 6U
#define AUDIO_TASK_CORE     0

/* ── Amostras WAV por chunk: 256 @ 16kHz = 16ms ─────────────────────────── */

#define WAV_SAMPLES_PER_CHUNK  NB_AUDIO_CHUNK_FRAMES

/* ── Cabeçalho WAV ───────────────────────────────────────────────────────── */

#define WAV_HDR_AUDIO_FORMAT_OFFSET  20U
#define WAV_HDR_NUM_CHANNELS_OFFSET  22U
#define WAV_HDR_SAMPLE_RATE_OFFSET   24U
#define WAV_HDR_BITS_PER_SAMPLE_OFFSET 34U
#define WAV_HDR_MIN_SIZE             44U

/* ── Estado interno ──────────────────────────────────────────────────────── */

typedef enum {
    PLAY_IDLE   = 0,
    PLAY_ACTIVE,
    PLAY_STOP,
} play_state_t;

typedef enum {
    VAD_SILENCE = 0,
    VAD_ACTIVE,
} vad_state_t;

typedef enum {
    REC_IDLE = 0,
    REC_ACTIVE,
} rec_state_t;

static struct {
    bool                 initialized;
    SemaphoreHandle_t    mutex;

    /* Callback para eventos (bridge para event bus no boot_manager) */
    nb_audio_event_cb_t  event_cb;

    /* Playback */
    volatile play_state_t play_state;
    char                 play_path[128];
    bool                 play_raw_pcm;   /* true = PCM raw (sem header WAV) */
    uint8_t              volume;         /* 0–100 */

    /* VAD */
    int32_t              vad_threshold;
    vad_state_t          vad_state;
    int64_t              vad_silence_start_us;
    uint8_t              vad_enter_count;    /* chunks consecutivos acima do threshold */
    uint32_t             vad_settle_ms;      /* > 0 = settling — não emite eventos VAD */
    uint32_t             vad_noise_log_ctr;  /* contador para log periódico de ruído */
    int32_t              vad_noise_peak;     /* pico de RMS em silêncio (janela atual) */

    /* Gravação de diagnóstico */
    rec_state_t          rec_state;
    char                 rec_path[128];
    uint32_t             rec_samples_remaining;
} s;

/* ── Buffers estáticos da task (SRAM) ────────────────────────────────────── */

static int32_t  s_mic_buf  [NB_AUDIO_CHUNK_FRAMES];
static int16_t  s_wav_chunk[WAV_SAMPLES_PER_CHUNK];
static int16_t  s_rec_chunk[NB_AUDIO_CHUNK_FRAMES];

/* ── Helpers WAV ─────────────────────────────────────────────────────────── */

static void wav_write_header(FILE *f, uint32_t data_bytes)
{
    const uint32_t sample_rate   = 16000U;
    const uint16_t num_channels  = 1U;
    const uint16_t bits_per_samp = 16U;
    const uint16_t block_align   = (uint16_t)(num_channels * bits_per_samp / 8U);
    const uint32_t byte_rate     = sample_rate * block_align;
    const uint32_t chunk_size    = 36U + data_bytes;
    const uint16_t audio_fmt     = 1U;  /* PCM */
    const uint32_t fmt_size      = 16U;

    fwrite("RIFF",        1, 4, f);
    fwrite(&chunk_size,   4, 1, f);
    fwrite("WAVE",        1, 4, f);
    fwrite("fmt ",        1, 4, f);
    fwrite(&fmt_size,     4, 1, f);
    fwrite(&audio_fmt,    2, 1, f);
    fwrite(&num_channels, 2, 1, f);
    fwrite(&sample_rate,  4, 1, f);
    fwrite(&byte_rate,    4, 1, f);
    fwrite(&block_align,  2, 1, f);
    fwrite(&bits_per_samp,2, 1, f);
    fwrite("data",        1, 4, f);
    fwrite(&data_bytes,   4, 1, f);
}

/*
 * Valida cabeçalho WAV e retorna offset do início dos dados PCM.
 * Retorna 0 em caso de erro ou formato não suportado.
 * Preenche *duration_ms se não NULL.
 */
static long wav_parse_header(FILE *f, uint32_t *duration_ms)
{
    uint8_t hdr[WAV_HDR_MIN_SIZE];
    if (fread(hdr, 1, WAV_HDR_MIN_SIZE, f) != WAV_HDR_MIN_SIZE) return 0;

    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return 0;

    uint16_t fmt_type, ch, bps;
    uint32_t sr;
    memcpy(&fmt_type, hdr + WAV_HDR_AUDIO_FORMAT_OFFSET, 2);
    memcpy(&ch,       hdr + WAV_HDR_NUM_CHANNELS_OFFSET, 2);
    memcpy(&sr,       hdr + WAV_HDR_SAMPLE_RATE_OFFSET,  4);
    memcpy(&bps,      hdr + WAV_HDR_BITS_PER_SAMPLE_OFFSET, 2);

    if (fmt_type != 1U || ch != 1U || sr != 16000U || bps != 16U) return 0;

    /* Busca sub-chunk 'data' a partir do byte 12 */
    fseek(f, 12, SEEK_SET);
    for (int guard = 0; guard < 16; guard++) {
        char     id[4];
        uint32_t sz;
        if (fread(id, 1, 4, f) != 4) return 0;
        if (fread(&sz, 4, 1, f) != 1) return 0;
        if (memcmp(id, "data", 4) == 0) {
            if (duration_ms) {
                /* 16kHz mono 16-bit: sz bytes / 2 bytes/sample / 16000 samples/s */
                *duration_ms = (sz / 2U * 1000U) / 16000U;
            }
            return ftell(f);
        }
        fseek(f, (long)((sz + 1U) & ~1U), SEEK_CUR);  /* skip, align to 2 */
    }
    return 0;
}

/* ── VAD ─────────────────────────────────────────────────────────────────── */

/* Duração aproximada de um chunk em ms (256 samples @ 16kHz = 16ms). */
#define CHUNK_DURATION_MS  16U

static void vad_update(const int32_t *mic, size_t n)
{
    if (n == 0) return;

    /* Settling pós-init: consome chunks silenciosamente até o mic estabilizar. */
    if (s.vad_settle_ms > 0) {
        s.vad_settle_ms = (s.vad_settle_ms > CHUNK_DURATION_MS)
                          ? s.vad_settle_ms - CHUNK_DURATION_MS : 0;
        if (s.vad_settle_ms == 0) {
            ESP_LOGI(TAG, "VAD settling completo — deteccao ativa");
        }
        return;
    }

    int64_t sum_sq = 0;
    uint32_t zcr_count = 0;
    for (size_t i = 0; i < n; i++) {
        int64_t v = mic[i];
        sum_sq += v * v;
        if (i > 0 && ((mic[i - 1] >= 0) != (mic[i] >= 0))) zcr_count++;
    }
    int32_t rms = (int32_t)sqrtf((float)(sum_sq / (int64_t)n));

    /* ZCR: taxa de cruzamentos de zero (fala humana ≈ 0.05–0.45).
     * Filtra ruído DC (ZCR≈0), chiados e fans (ZCR>0.45). */
    float zcr = (n > 1u) ? ((float)zcr_count / (float)(n - 1u)) : 0.0f;
    bool is_speech = (rms > s.vad_threshold) && (zcr >= 0.05f) && (zcr <= 0.45f);

    int64_t now_us = esp_timer_get_time();

    if (is_speech) {
        if (s.vad_state == VAD_SILENCE) {
            if (++s.vad_enter_count >= VAD_ENTER_CHUNKS) {
                s.vad_enter_count = 0;
                s.vad_state = VAD_ACTIVE;
                s.vad_silence_start_us = 0;
                if (s.event_cb) s.event_cb(NB_AUDIO_EVT_VOICE_START, 0);
                ESP_LOGI(TAG, "VAD START rms=%ld", (long)rms);
            }
        } else {
            s.vad_enter_count = 0;
            s.vad_silence_start_us = 0;  /* reinicia contador de silêncio */
        }
    } else {
        s.vad_enter_count = 0;
        /* RMS alto mas ZCR fora da faixa de fala (ruído, fan, música) — ignora. */
        /* Log periódico do pico de ruído ambiente para calibração. */
        if (s.vad_state == VAD_SILENCE) {
            if (rms > s.vad_noise_peak) s.vad_noise_peak = rms;
            if (++s.vad_noise_log_ctr >= VAD_NOISE_LOG_CHUNKS) {
                ESP_LOGI(TAG, "VAD noise peak=%ld thr=%ld",
                         (long)s.vad_noise_peak, (long)s.vad_threshold);
                s.vad_noise_log_ctr = 0;
                s.vad_noise_peak    = 0;
            }
        }
        if (s.vad_state == VAD_ACTIVE) {
            if (s.vad_silence_start_us == 0) {
                s.vad_silence_start_us = now_us;
            } else {
                int64_t silence_ms = (now_us - s.vad_silence_start_us) / 1000LL;
                if (silence_ms >= (int64_t)NB_AUDIO_VAD_SILENCE_MS) {
                    s.vad_state = VAD_SILENCE;
                    if (s.event_cb) s.event_cb(NB_AUDIO_EVT_VOICE_END, 0);
                    ESP_LOGI(TAG, "VAD END silence=%lldms", (long long)silence_ms);
                }
            }
        }
    }
}

/* ── audio_task ──────────────────────────────────────────────────────────── */

static void audio_task(void *arg)
{
    (void)arg;
    FILE *wav_file = NULL;
    FILE *rec_file = NULL;

    ESP_LOGI(TAG, "audio_task iniciada");

    while (1) {
        /* ── 1. Chunk TX ─────────────────────────────────────────────────── */
        play_state_t play_state;
        xSemaphoreTake(s.mutex, portMAX_DELAY);
        play_state = s.play_state;
        xSemaphoreGive(s.mutex);

        bool wrote_audio = false;

        /* ── Stop pedido ── */
        if (play_state == PLAY_STOP) {
            if (wav_file) {
                fclose(wav_file);
                wav_file = NULL;
            }
            if (s.event_cb) s.event_cb(NB_AUDIO_EVT_PLAYBACK_END, 0);
            xSemaphoreTake(s.mutex, portMAX_DELAY);
            s.play_state = PLAY_IDLE;
            xSemaphoreGive(s.mutex);
        }

        /* ── Reprodução ativa ── */
        else if (play_state == PLAY_ACTIVE) {
            /* Abrir arquivo na primeira vez */
            if (!wav_file) {
                char path[128];
                bool raw_pcm;
                xSemaphoreTake(s.mutex, portMAX_DELAY);
                memcpy(path, s.play_path, sizeof(path));
                raw_pcm = s.play_raw_pcm;
                xSemaphoreGive(s.mutex);

                wav_file = fopen(path, "rb");
                if (!wav_file) {
                    ESP_LOGW(TAG, "asset ausente: %s", path);
                    xSemaphoreTake(s.mutex, portMAX_DELAY);
                    s.play_state = PLAY_IDLE;
                    xSemaphoreGive(s.mutex);
                } else if (raw_pcm) {
                    /* PCM raw: sem header, começa no byte 0 */
                    long file_size = 0;
                    fseek(wav_file, 0, SEEK_END);
                    file_size = ftell(wav_file);
                    fseek(wav_file, 0, SEEK_SET);
                    uint32_t duration_ms = (uint32_t)((file_size / 2U * 1000U) / 16000U);
                    if (s.event_cb) s.event_cb(NB_AUDIO_EVT_PLAYBACK_START, duration_ms);
                    ESP_LOGI(TAG, "reproduzindo PCM raw: %s (%ums)", path, (unsigned)duration_ms);
                } else {
                    uint32_t duration_ms = 0;
                    long data_off = wav_parse_header(wav_file, &duration_ms);
                    if (data_off <= 0) {
                        ESP_LOGE(TAG, "formato WAV invalido: %s", path);
                        fclose(wav_file);
                        wav_file = NULL;
                        xSemaphoreTake(s.mutex, portMAX_DELAY);
                        s.play_state = PLAY_IDLE;
                        xSemaphoreGive(s.mutex);
                    } else {
                        fseek(wav_file, data_off, SEEK_SET);
                        if (s.event_cb) s.event_cb(NB_AUDIO_EVT_PLAYBACK_START, duration_ms);
                        ESP_LOGI(TAG, "reproduzindo: %s (%ums)", path, (unsigned)duration_ms);
                    }
                }
            }

            if (wav_file) {
                size_t n = fread(s_wav_chunk, sizeof(int16_t), WAV_SAMPLES_PER_CHUNK, wav_file);
                if (n == 0) {
                    /* EOF */
                    fclose(wav_file);
                    wav_file = NULL;
                    if (s.event_cb) s.event_cb(NB_AUDIO_EVT_PLAYBACK_END, 0);
                    ESP_LOGI(TAG, "reproducao encerrada (EOF)");
                    xSemaphoreTake(s.mutex, portMAX_DELAY);
                    s.play_state = PLAY_IDLE;
                    xSemaphoreGive(s.mutex);
                } else {
                    /* Aplicar volume: mult = volume * 256 / 100 */
                    uint32_t mult = ((uint32_t)s.volume * 256U) / 100U;
                    for (size_t i = 0; i < n; i++) {
                        int32_t v = ((int32_t)s_wav_chunk[i] * (int32_t)mult) >> 8;
                        if (v >  32767) v =  32767;
                        if (v < -32768) v = -32768;
                        s_wav_chunk[i] = (int16_t)v;
                    }
                    if (n < WAV_SAMPLES_PER_CHUNK) {
                        memset(s_wav_chunk + n, 0,
                               (WAV_SAMPLES_PER_CHUNK - n) * sizeof(int16_t));
                    }
                    audio_hal_spk_write(s_wav_chunk, WAV_SAMPLES_PER_CHUNK,
                                        pdMS_TO_TICKS(100));
                    wrote_audio = true;
                }
            }
        }

        /* Silêncio se não tocou áudio */
        if (!wrote_audio) {
            audio_hal_spk_write_silence(NB_AUDIO_CHUNK_FRAMES, pdMS_TO_TICKS(100));
        }

        /* ── 2. Chunk RX ─────────────────────────────────────────────────── */
        size_t mic_n = 0;
        esp_err_t rc = audio_hal_mic_read(s_mic_buf, NB_AUDIO_CHUNK_FRAMES,
                                           &mic_n, pdMS_TO_TICKS(100));
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "mic_read err: %s", esp_err_to_name(rc));
            continue;
        }

        /* ── 3. VAD ─────────────────────────────────────────────────────── */
        vad_update(s_mic_buf, mic_n);

        /* ── 4. Diagnóstico ─────────────────────────────────────────────── */
        if (s.rec_state == REC_ACTIVE) {
            if (!rec_file) {
                rec_file = fopen(s.rec_path, "wb");
                if (!rec_file) {
                    ESP_LOGE(TAG, "rec fopen falhou: %s", s.rec_path);
                    s.rec_state = REC_IDLE;
                } else {
                    uint32_t data_bytes = s.rec_samples_remaining * sizeof(int16_t);
                    wav_write_header(rec_file, data_bytes);
                    ESP_LOGI(TAG, "gravacao iniciada: %s", s.rec_path);
                }
            }

            if (rec_file && s.rec_samples_remaining > 0) {
                size_t to_write = mic_n < s.rec_samples_remaining
                                  ? mic_n : (size_t)s.rec_samples_remaining;
                for (size_t i = 0; i < to_write; i++) {
                    /*
                     * s_mic_buf[i]: valor 24-bit (audio_hal já fez >> 8 do raw 32-bit).
                     * Shift >> 8 para descer ao range 16-bit antes do clamp.
                     * Pico típico de voz direta ~400K–800K → ~1500–3000 em 16-bit.
                     */
                    int32_t v = (s_mic_buf[i] >> 8) << 3;  /* +18 dB de ganho de gravação */
                    if (v >  32767) v =  32767;
                    if (v < -32768) v = -32768;
                    s_rec_chunk[i] = (int16_t)v;
                }
                fwrite(s_rec_chunk, sizeof(int16_t), to_write, rec_file);
                s.rec_samples_remaining -= (uint32_t)to_write;

                if (s.rec_samples_remaining == 0) {
                    fclose(rec_file);
                    rec_file = NULL;
                    s.rec_state = REC_IDLE;
                    ESP_LOGI(TAG, "gravacao concluida: %s", s.rec_path);
                }
            }
        }
    }
}

/* ── API pública ─────────────────────────────────────────────────────────── */

esp_err_t audio_service_init(void)
{
    if (s.initialized) return ESP_ERR_INVALID_STATE;

    esp_err_t err = audio_hal_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_hal_init falhou: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    s.mutex = xSemaphoreCreateMutex();
    if (!s.mutex) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex falhou");
        audio_hal_deinit();
        return ESP_ERR_NO_MEM;
    }

    s.volume         = 80U;  /* Default; boot_manager ajusta via audio_set_volume */
    s.vad_threshold  = NB_AUDIO_VAD_THRESHOLD_DEFAULT;
    s.vad_state      = VAD_SILENCE;
    s.vad_settle_ms  = NB_AUDIO_VAD_SETTLE_MS;
    s.play_state     = PLAY_IDLE;
    s.rec_state      = REC_IDLE;

    BaseType_t rc = xTaskCreatePinnedToCore(
        audio_task, "audio_task",
        AUDIO_TASK_STACK, NULL,
        AUDIO_TASK_PRIORITY, NULL,
        AUDIO_TASK_CORE
    );
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore audio_task falhou");
        vSemaphoreDelete(s.mutex);
        audio_hal_deinit();
        return ESP_ERR_NO_MEM;
    }

    s.initialized = true;
    ESP_LOGI(TAG, "inicializado (vol=%u, vad_thr=%ld)",
             (unsigned)s.volume, (long)s.vad_threshold);
    return ESP_OK;
}

void audio_service_set_event_cb(nb_audio_event_cb_t cb)
{
    s.event_cb = cb;
}

esp_err_t audio_play_file(const char *path)
{
    if (!s.initialized || !path) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    snprintf(s.play_path, sizeof(s.play_path), "%s", path);
    s.play_raw_pcm = false;
    s.play_state   = PLAY_ACTIVE;
    xSemaphoreGive(s.mutex);
    return ESP_OK;
}

esp_err_t audio_play_pcm_raw(const char *path)
{
    if (!s.initialized || !path) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    snprintf(s.play_path, sizeof(s.play_path), "%s", path);
    s.play_raw_pcm = true;
    s.play_state   = PLAY_ACTIVE;
    xSemaphoreGive(s.mutex);
    return ESP_OK;
}

esp_err_t audio_play_stop(void)
{
    if (!s.initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    if (s.play_state == PLAY_ACTIVE) s.play_state = PLAY_STOP;
    xSemaphoreGive(s.mutex);
    return ESP_OK;
}

bool audio_is_playing(void)
{
    return s.initialized && (s.play_state == PLAY_ACTIVE);
}

esp_err_t audio_set_volume(uint8_t level)
{
    if (level > 100U) return ESP_ERR_INVALID_ARG;
    s.volume = level;
    return ESP_OK;
}

uint8_t audio_get_volume(void)
{
    return s.volume;
}

void audio_service_set_vad_threshold(int32_t threshold)
{
    s.vad_threshold = threshold;
}

esp_err_t audio_record_diagnostic(const char *path, uint32_t duration_s)
{
    if (!s.initialized || !path)          return ESP_ERR_INVALID_STATE;
    if (s.rec_state == REC_ACTIVE)        return ESP_ERR_INVALID_STATE;
    if (duration_s == 0 || duration_s > 10U) return ESP_ERR_INVALID_ARG;

    snprintf(s.rec_path, sizeof(s.rec_path), "%s", path);
    s.rec_samples_remaining = duration_s * 16000U;
    s.rec_state = REC_ACTIVE;

    ESP_LOGI(TAG, "gravacao agendada: %s (%us)", path, (unsigned)duration_s);
    return ESP_OK;
}
