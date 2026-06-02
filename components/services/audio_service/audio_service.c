/*
 * audio_service.c — Serviço de áudio do NoiseBot (Layer 4)
 *
 * Mantém ownership do HAL/I2S. Eventos são emitidos via callback
 * (nb_audio_event_cb_t) registrado pelo boot_manager, que faz a ponte para o
 * event bus. Integrações de infra ficam restritas aos contratos de bridge e
 * config já existentes, sem event_bus direto no caminho crítico.
 *
 * Loop da task (~16ms por iteração):
 *   1. Preparar e escrever chunk TX (áudio WAV ou silêncio) — manter DMA alimentado.
 *   2. Ler chunk RX do microfone.
 *   3. Atualizar VAD: RMS sobre janela de NB_AUDIO_CHUNK_FRAMES amostras.
 *   4. Gravação de diagnóstico: escrever amostras no arquivo WAV se ativo.
 */

#include "audio_service.h"
#include "audio_hal.h"
#include "sound_analysis_service.h"
#include "synth_service.h"
#include "bridge_service.h"
#include "wake_service.h"
#include "audio_playback_service_v2.h"
#include "audio_codec_service_v2.h"
#include "audio_processor_service.h"
#include "audio_io_service_v2.h"
#include "voice_activity_service_v2.h"
#include "voice_capture_session_v2.h"
#include "config_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nb_events.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define TAG "audio_svc"

/* ESP-SR VAD: declaracoes minimas para evitar conflito de nomes com o VAD
 * heuristico local (esp_vad.h tambem define VAD_SILENCE/vad_state_t). */
typedef void *nb_esp_vad_handle_t;
extern nb_esp_vad_handle_t vad_create_with_param(
    int vad_mode, int sample_rate, int one_frame_ms, int min_speech_ms, int min_noise_ms);
extern int vad_process(nb_esp_vad_handle_t handle, int16_t *data,
                       int sample_rate_hz, int one_frame_ms);
extern void vad_reset_trigger(nb_esp_vad_handle_t handle);
extern void vad_destroy(nb_esp_vad_handle_t handle);

#define ESP_SR_VAD_MODE             3   /* VAD_MODE_3: agressivo, estilo always-on */
#define ESP_SR_VAD_FRAME_MS        10
#define ESP_SR_VAD_FRAME_SAMPLES  160
#define ESP_SR_VAD_MIN_SPEECH_MS   80
#define ESP_SR_VAD_MIN_NOISE_MS   500
#define ESP_SR_VAD_SPEECH           1

/* 12.11: a heuristica local permanece para diagnostico/calibracao. Listening
 * conversacional deve depender do ESP-SR VAD; este fallback so existe para
 * bancada, quando explicitamente habilitado antes do build. */
#ifndef NB_AUDIO_HEURISTIC_LISTEN_FALLBACK
#define NB_AUDIO_HEURISTIC_LISTEN_FALLBACK 0
#endif

#define NB_AUDIO_EVT_DATA_SESSION 1U

/* Pontuação para declarar VAD START (5 = ~80ms de fala forte ou ~160ms suave).
 * Frames fortes somam 2, soft somam 1, não-fala desconta 1. */
#define VAD_ENTER_SCORE_START       5U
#define VAD_ENTER_SCORE_STRONG_INC  2U
#define VAD_ENTER_SCORE_SOFT_INC    1U
#define VAD_ENTER_SCORE_DEC         1U

/* Log de ruído ambiente a cada N chunks quando em silêncio (~8s). */
#define VAD_NOISE_LOG_CHUNKS 500U

/* Gate near-field: só abre VAD quando a energia parece fala próxima, não
 * ambiente distante. */
#define VAD_MIN_RMS_ABS           24000.0f
#define VAD_MIN_RMS_SOFT          18000.0f
#define VAD_LISTEN_MIN_RMS_ABS    12000.0f
#define VAD_LISTEN_MIN_RMS_SOFT    9000.0f
#define VAD_NOISE_MULT_ENTER          1.35f
#define VAD_NOISE_MULT_SOFT           1.05f
#define VAD_NOISE_MULT_ACTIVE         1.6f
#define VAD_LISTEN_NOISE_MULT_ENTER   1.10f
#define VAD_LISTEN_NOISE_MULT_SOFT    0.90f
#define VAD_LISTEN_NOISE_MULT_ACTIVE  1.25f
#define VAD_ZCR_MIN                  0.04f
#define VAD_ZCR_MAX                  0.26f
#define VAD_VOICE_RATIO_MIN          0.55f
#define VAD_VOICE_MID_RATIO_MIN      0.14f
#define VAD_HIGH_RATIO_MAX           0.34f
#define VAD_LOW_RATIO_MAX            0.48f
#define VAD_DOM_FREQ_MIN_HZ        250.0f
#define VAD_ENGINE_DOM_MAX_HZ      625.0f
#define VAD_ENGINE_ZCR_MAX           0.14f
#define VAD_ENGINE_HIGH_RATIO_MAX    0.03f
#define VAD_ENGINE_LOW_DOMINANCE     1.35f
#define VAD_ENGINE_RMS_MIN       55000.0f
#define VAD_PLAYBACK_MUTE_MS       600U

/* Sessão de escuta: turn-taking explícito. A janela curta vale só antes da
 * primeira fala; depois que começou, encerra por silêncio pós-fala ou teto de
 * segurança para ruído contínuo. */
#define LISTEN_WAIT_SPEECH_TIMEOUT_MS   8000U
#define LISTEN_END_SILENCE_MS            900U
#define LISTEN_EARLY_END_SILENCE_MS     1300U
#define LISTEN_EARLY_GRACE_MS           6000U
/* Contrato conversacional v1: a fala útil é curta e previsível. Mantemos o
 * teto do firmware abaixo da folga do server (192000 samples @16kHz = 12s)
 * para o VOICE_END chegar antes do descarte por audio_longo. */
#define LISTEN_MAX_SPEECH_MS            9200U
#define BRIDGE_TX_FAIL_ABORT_COUNT     4U

/* High-pass one-pole para tirar rumble/engine abaixo de ~180 Hz antes do VAD.
 * alpha = RC / (RC + dt), fs=16kHz, fc≈180Hz. */
#define MIC_HPF_ALPHA             0.934f

/* Bridge TX: AGC simples só no caminho enviado ao STT.
 * Voz média ganha corpo; voz alta é atenuada antes de clipar. */
#define BRIDGE_TX_TARGET_PEAK    12000
#define BRIDGE_TX_MAX_GAIN_Q8     4096   /* 16.0x */
#define BRIDGE_TX_MIN_GAIN_Q8       64   /* 0.25x */
#define BRIDGE_TX_LOG_CHUNKS       125U /* ~2s em 16ms/chunk */

/* ── Configuração da task ────────────────────────────────────────────────── */

#define AUDIO_TASK_STACK    4096U
#define AUDIO_TASK_PRIORITY 6U
#define AUDIO_TASK_CORE     0

#define AUDIO_HAL_RECOVER_FAILS  3U
#define AUDIO_HAL_RECOVER_DELAY_MS 40U

/* ── Amostras WAV por chunk: 256 @ 16kHz = 16ms ─────────────────────────── */

#define WAV_SAMPLES_PER_CHUNK  NB_AUDIO_CHUNK_FRAMES

/* Pre-roll: 20 chunks × 16ms = 320ms. Dá folga para capturar o começo da fala
 * quando o usuário emenda a frase imediatamente após o wake word. */
#define PREROLL_CHUNKS         20U

/* ── Cabeçalho WAV ───────────────────────────────────────────────────────── */

#define WAV_HDR_AUDIO_FORMAT_OFFSET  20U
#define WAV_HDR_NUM_CHANNELS_OFFSET  22U
#define WAV_HDR_SAMPLE_RATE_OFFSET   24U
#define WAV_HDR_BITS_PER_SAMPLE_OFFSET 34U
#define WAV_HDR_MIN_SIZE             44U

/* ── Estado interno ──────────────────────────────────────────────────────── */

typedef enum {
    PLAY_IDLE        = 0,
    PLAY_ACTIVE,
    PLAY_STOP,
    PLAY_BRIDGE_SAY, /* reproduzindo chunks PCM vindos do bridge */
} play_state_t;

typedef enum {
    VAD_SILENCE = 0,
    VAD_ACTIVE,
} vad_state_t;

typedef enum {
    REC_IDLE = 0,
    REC_ACTIVE,
} rec_state_t;

typedef enum {
    LISTEN_PHASE_IDLE = 0,
    LISTEN_PHASE_WAITING_FOR_SPEECH,
    LISTEN_PHASE_CAPTURING_SPEECH,
    LISTEN_PHASE_ENDING_ON_SILENCE,
} listen_phase_t;

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
    uint8_t              vad_enter_count;    /* pontuação acumulada para abrir VAD */
    uint32_t             vad_settle_ms;      /* > 0 = settling — não emite eventos VAD */
    int32_t              vad_noise_peak;     /* pico de RMS em silêncio (janela atual) */
    float                vad_noise_ema;      /* média exponencial do ruído de fundo */
    uint32_t             vad_playback_mute_ms; /* inibe VAD após áudio local */
    nb_esp_vad_handle_t  esp_vad;            /* ESP-SR VAD para listening */
    bool                 esp_vad_enabled;
    uint16_t             esp_vad_pos;
    int16_t              esp_vad_frame[ESP_SR_VAD_FRAME_SAMPLES];
    int                  esp_vad_last_state;

    /* Gravação de diagnóstico */
    rec_state_t          rec_state;
    char                 rec_path[128];
    uint32_t             rec_samples_remaining;
    bool                 rec_bridge_tx_source;

    /* Bridge (Etapa 12.2) */
    bool                 bridge_tx_active;   /* true = streaming mic para bridge */
    bool                 bridge_say_playing; /* true = play_state == PLAY_BRIDGE_SAY */
    uint32_t             bridge_say_empty_ms;/* tolera jitter antes de encerrar  */

    /* Sessão de escuta (Etapa 12.3) */
    bool                 listen_session_active;       /* sessão aberta          */
    bool                 bridge_start_sent;           /* VOICE_START foi à bridge */
    bool                 bridge_audio_sent;           /* ao menos 1 chunk enviado */
    uint32_t             listen_legacy_tx_frames;     /* chunks/pacotes enviados */
    uint32_t             listen_legacy_tx_samples;    /* samples equivalentes enviados */
    uint8_t              bridge_tx_fail_count;        /* falhas seguidas na fila TX */
    bool                 bridge_flush_before_end;     /* limpa fila antes do END    */
    listen_phase_t       listen_phase;                /* turn-taking interno       */
    nb_listen_mode_t     listen_mode;                 /* auto/manual/realtime      */
    uint32_t             listen_wait_remaining_ms;    /* espera por 1a fala        */
    uint32_t             listen_speech_elapsed_ms;    /* teto desde fala iniciar   */
    bool                 listen_voice_detected;       /* VAD ativou na sessão    */
    bool                 listen_skip_preroll;         /* barge-in nao envia TTS antigo */
    bool                 listen_capture_v2_active;    /* contrato v2 opt-in ativo */
    bool                 listen_capture_v2_tx_owner;  /* v2 envia VOICE_START/CHUNK/END */
} s;

#define BRIDGE_SAY_IDLE_END_MS  1200U
static uint32_t         s_bridge_say_drop_count;
static uint32_t         s_bridge_say_rx_count;
static uint32_t         s_bridge_say_play_count;

/* ── Pre-roll ring buffer (SRAM, 10 KB) ──────────────────────────────────── */

static int16_t  s_preroll_buf[PREROLL_CHUNKS][NB_AUDIO_CHUNK_FRAMES];
static uint8_t  s_preroll_head;   /* próxima posição de escrita */
static uint8_t  s_preroll_count;  /* chunks válidos (0..PREROLL_CHUNKS) */

/* ── Buffers estáticos da task (SRAM) ────────────────────────────────────── */

static int32_t  s_mic_buf  [NB_AUDIO_CHUNK_FRAMES];
static int32_t  s_mic_proc [NB_AUDIO_CHUNK_FRAMES]; /* mic após high-pass */
static int16_t  s_wav_chunk[WAV_SAMPLES_PER_CHUNK];
static int16_t  s_rec_chunk[NB_AUDIO_CHUNK_FRAMES];
static int16_t  s_sa_buf   [NB_AUDIO_CHUNK_FRAMES]; /* buffer para sound_analysis_tick */
static int16_t  s_wake_buf [NB_AUDIO_CHUNK_FRAMES]; /* mic cru 16-bit para WakeNet */
static int16_t  s_bridge_buf[NB_AUDIO_CHUNK_FRAMES]; /* mic ganho/limit para bridge */
static int16_t  s_bridge_proc_buf[NB_AUDIO_CHUNK_FRAMES]; /* saida AFE opcional */
static uint8_t  s_opus_packet_buf[NB_BRIDGE_OPUS_MAX_PACKET_BYTES];
static uint32_t s_bridge_diag_chunk_counter = 0;
static uint32_t s_spk_fail_count = 0;
static uint32_t s_mic_fail_count = 0;

static void esp_vad_reset(void);

static bool bridge_use_codec_v2_opus_worker(void)
{
    nb_audio_codec_v2_status_t st;
    audio_codec_service_v2_get_status(&st);
    return st.worker_active ||
           st.worker_state == NB_AUDIO_CODEC_V2_WORKER_STATE_STARTING ||
           st.worker_state == NB_AUDIO_CODEC_V2_WORKER_STATE_RUNNING;
}

static void bridge_feed_opus_pcm(const int16_t *pcm, uint16_t samples)
{
    if (bridge_use_codec_v2_opus_worker()) {
        (void)audio_codec_service_v2_feed_pcm16(pcm, samples);
    } else {
        audio_processor_service_opus_worker_feed_pcm(pcm, samples);
    }
}

static uint8_t bridge_drain_opus_packets_if_enabled(bool capture_v2_tx_owner)
{
    if (!bridge_service_opus_is_enabled()) {
        return 0;
    }

    bool use_codec_v2 = bridge_use_codec_v2_opus_worker();
    uint8_t sent_packets = 0;
    for (uint8_t i = 0; i < 4U; i++) {
        uint16_t packet_len = 0;
        esp_err_t read_rc = use_codec_v2
            ? audio_codec_service_v2_read_opus_packet(s_opus_packet_buf, sizeof(s_opus_packet_buf), &packet_len)
            : audio_processor_service_opus_worker_read_packet(
                s_opus_packet_buf, sizeof(s_opus_packet_buf), &packet_len);
        if (read_rc != ESP_OK) {
            break;
        }
        esp_err_t tx_rc = capture_v2_tx_owner
            ? voice_capture_session_v2_send_opus_packet(s_opus_packet_buf, packet_len)
            : bridge_service_send_opus_packet(s_opus_packet_buf, packet_len);
        if (tx_rc != ESP_OK) {
            break;
        }
        sent_packets++;
    }
    return sent_packets;
}

static void audio_service_recover_hal(const char *where, esp_err_t err)
{
    ESP_LOGW(TAG, "recuperando I2S apos falhas em %s: %s",
             where ? where : "audio", esp_err_to_name(err));
    audio_io_service_v2_note_i2s_recovery(err);
    audio_hal_deinit();
    vTaskDelay(pdMS_TO_TICKS(AUDIO_HAL_RECOVER_DELAY_MS));
    esp_err_t rc = audio_hal_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "audio_hal_init durante recuperacao falhou: %s",
                 esp_err_to_name(rc));
    } else {
        ESP_LOGI(TAG, "I2S recuperado por software");
    }
    s_spk_fail_count = 0;
    s_mic_fail_count = 0;
    esp_vad_reset();
}

static void audio_note_spk_result(esp_err_t err, const char *where,
                                  uint16_t sample_count, bool silence)
{
    audio_io_service_v2_tx_owner_note_frame(sample_count, silence, err);
    if (err == ESP_OK) {
        s_spk_fail_count = 0;
        return;
    }

    s_spk_fail_count++;
    ESP_LOGW(TAG, "speaker write falhou em %s: %s (falhas=%lu/%u)",
             where ? where : "audio", esp_err_to_name(err),
             (unsigned long)s_spk_fail_count, (unsigned)AUDIO_HAL_RECOVER_FAILS);
    if (s_spk_fail_count >= AUDIO_HAL_RECOVER_FAILS) {
        audio_service_recover_hal(where, err);
    }
}

static esp_err_t audio_service_playback_v2_write_speaker(const int16_t *samples,
                                                         uint16_t sample_count,
                                                         void *ctx)
{
    (void)ctx;
    return audio_hal_spk_write(samples, sample_count, pdMS_TO_TICKS(100));
}

static void audio_note_mic_result(esp_err_t err)
{
    if (err == ESP_OK) {
        s_mic_fail_count = 0;
        return;
    }

    s_mic_fail_count++;
    ESP_LOGW(TAG, "mic_read err: %s (falhas=%lu/%u)",
             esp_err_to_name(err), (unsigned long)s_mic_fail_count,
             (unsigned)AUDIO_HAL_RECOVER_FAILS);
    if (s_mic_fail_count >= AUDIO_HAL_RECOVER_FAILS) {
        audio_service_recover_hal("mic_read", err);
    }
}

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

/*
 * Remove offset DC + rumble antes de VAD/FFT/bridge.
 *
 * Os logs em silêncio mostraram RMS alto com ZCR≈0, típico de offset ou
 * rumble de baixa frequência no caminho I2S/mic. Carros e motos também
 * chegam fortes em 62-188Hz; o high-pass evita que esse grave domine a FFT
 * e suba o threshold adaptativo.
 */
static void mic_condition_signal(const int32_t *in, int32_t *out, size_t n)
{
    if (n == 0U) return;

    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += (int64_t)in[i];
    }

    int32_t mean = (int32_t)(sum / (int64_t)n);

    static float prev_x = 0.0f;
    static float prev_y = 0.0f;

    for (size_t i = 0; i < n; i++) {
        float x = (float)(in[i] - mean);
        float y = MIC_HPF_ALPHA * (prev_y + x - prev_x);
        prev_x = x;
        prev_y = y;
        out[i] = (int32_t)y;
    }
}

/* ── VAD ─────────────────────────────────────────────────────────────────── */

/* Duração aproximada de um chunk em ms (256 samples @ 16kHz = 16ms). */
#define CHUNK_DURATION_MS  16U

static const char *listen_phase_name(listen_phase_t phase)
{
    switch (phase) {
    case LISTEN_PHASE_WAITING_FOR_SPEECH: return "waiting_for_speech";
    case LISTEN_PHASE_CAPTURING_SPEECH:   return "capturing_speech";
    case LISTEN_PHASE_ENDING_ON_SILENCE:  return "ending_on_silence";
    default:                              return "idle";
    }
}

static const char *listen_mode_name(nb_listen_mode_t mode)
{
    switch (mode) {
    case NB_LISTEN_MODE_AUTO:     return "auto";
    case NB_LISTEN_MODE_MANUAL:   return "manual";
    case NB_LISTEN_MODE_REALTIME: return "realtime";
    default:                      return "unknown";
    }
}

static const char *listen_end_reason_name(nb_listen_end_reason_t reason)
{
    switch (reason) {
    case NB_LISTEN_END_VAD_SILENCE:         return "silence";
    case NB_LISTEN_END_TIMEOUT:             return "timeout";
    case NB_LISTEN_END_BRIDGE_DISCONNECTED: return "bridge_disconnected";
    case NB_LISTEN_END_CANCELLED:           return "cancelled";
    default:                                return "unknown";
    }
}

static void bridge_prepare_tx_audio(const int16_t *in, int16_t *out, size_t n,
                                    int32_t *raw_rms_out, uint16_t *raw_peak_out,
                                    int32_t *tx_rms_out, uint16_t *tx_peak_out,
                                    uint16_t *sat_out)
{
    if (!in || !out || n == 0U) {
        if (raw_rms_out) *raw_rms_out = 0;
        if (raw_peak_out) *raw_peak_out = 0;
        if (tx_rms_out) *tx_rms_out = 0;
        if (tx_peak_out) *tx_peak_out = 0;
        if (sat_out) *sat_out = 0;
        return;
    }

    int64_t raw_sum_sq = 0;
    uint16_t raw_peak = 0;

    for (size_t i = 0; i < n; i++) {
        int32_t raw16 = in[i];
        uint16_t raw_abs = (uint16_t)(raw16 < 0 ? -raw16 : raw16);
        if (raw_abs > raw_peak) raw_peak = raw_abs;
        raw_sum_sq += (int64_t)raw16 * (int64_t)raw16;
    }

    int32_t gain_q8 = 256;
    if (raw_peak > 0U) {
        gain_q8 = (int32_t)(((int64_t)BRIDGE_TX_TARGET_PEAK * 256LL) / (int64_t)raw_peak);
        if (gain_q8 > BRIDGE_TX_MAX_GAIN_Q8) gain_q8 = BRIDGE_TX_MAX_GAIN_Q8;
        if (gain_q8 < BRIDGE_TX_MIN_GAIN_Q8) gain_q8 = BRIDGE_TX_MIN_GAIN_Q8;
    }

    int64_t tx_sum_sq = 0;
    uint16_t tx_peak = 0;
    uint16_t saturated = 0;

    for (size_t i = 0; i < n; i++) {
        int32_t raw16 = in[i];
        int32_t tx = (int32_t)(((int64_t)raw16 * (int64_t)gain_q8) >> 8);
        if (tx > 32767) {
            tx = 32767;
            saturated++;
        }
        if (tx < -32768) {
            tx = -32768;
            saturated++;
        }
        out[i] = (int16_t)tx;
        uint16_t tx_abs = (uint16_t)(tx < 0 ? -tx : tx);
        if (tx_abs > tx_peak) tx_peak = tx_abs;
        tx_sum_sq += (int64_t)tx * (int64_t)tx;
    }

    if (raw_rms_out) *raw_rms_out = (int32_t)sqrtf((float)(raw_sum_sq / (int64_t)n));
    if (raw_peak_out) *raw_peak_out = raw_peak;
    if (tx_rms_out) *tx_rms_out = (int32_t)sqrtf((float)(tx_sum_sq / (int64_t)n));
    if (tx_peak_out) *tx_peak_out = tx_peak;
    if (sat_out) *sat_out = saturated;
}

static const char *listen_source_name(nb_listen_source_t source)
{
    switch (source) {
    case NB_LISTEN_SOURCE_TOUCH:     return "touch";
    case NB_LISTEN_SOURCE_WAKE_WORD: return "wake_word";
    case NB_LISTEN_SOURCE_FOLLOWUP:  return "followup";
    case NB_LISTEN_SOURCE_BARGE_IN:  return "barge_in";
    case NB_LISTEN_SOURCE_DEBUG:     return "debug";
    default:                         return "unknown";
    }
}

static uint32_t listen_current_end_silence_ms(void)
{
    return (s.listen_speech_elapsed_ms < LISTEN_EARLY_GRACE_MS)
         ? LISTEN_EARLY_END_SILENCE_MS
         : LISTEN_END_SILENCE_MS;
}

static void esp_vad_reset(void)
{
    s.esp_vad_pos = 0;
    s.esp_vad_last_state = 0;
    if (s.esp_vad_enabled && s.esp_vad) {
        vad_reset_trigger(s.esp_vad);
    }
}

static int esp_vad_update(const int16_t *pcm, size_t n, bool muted)
{
    if (!s.esp_vad_enabled || !s.esp_vad || !pcm || n == 0 || muted) {
        if (muted) esp_vad_reset();
        return 0;
    }

    size_t off = 0;
    while (off < n) {
        size_t space = ESP_SR_VAD_FRAME_SAMPLES - s.esp_vad_pos;
        size_t to_copy = (n - off < space) ? (n - off) : space;
        memcpy(&s.esp_vad_frame[s.esp_vad_pos], &pcm[off],
               to_copy * sizeof(int16_t));
        s.esp_vad_pos += (uint16_t)to_copy;
        off += to_copy;

        if (s.esp_vad_pos >= ESP_SR_VAD_FRAME_SAMPLES) {
            s.esp_vad_last_state = vad_process(s.esp_vad, s.esp_vad_frame,
                                                16000, ESP_SR_VAD_FRAME_MS);
            s.esp_vad_pos = 0;
        }
    }

    return s.esp_vad_last_state;
}

static void vad_update(const int32_t *mic, const int16_t *pcm16, size_t n,
                       bool local_output_active);

typedef struct {
    bool io_v2_session_context;
    bool activity_session_context;
    bool activity_playback_context;
    bool activity_bridge_say_context;
    bool local_output_active;
    const int32_t *mic_samples;
    const int16_t *wake_samples;
} rx_dispatch_context_t;

static void rx_dispatch_sound_analysis_cb(const nb_audio_io_v2_pcm_frame_t *frame,
                                          void *ctx)
{
    (void)ctx;
    sound_analysis_tick(frame->samples, frame->sample_count);
}

static void rx_dispatch_processor_shadow_cb(const nb_audio_io_v2_pcm_frame_t *frame,
                                            void *ctx)
{
    (void)ctx;
    audio_processor_service_feed_shadow(frame->samples, frame->sample_count);
}

static void rx_dispatch_io_probe_cb(const nb_audio_io_v2_pcm_frame_t *frame,
                                    void *ctx)
{
    (void)ctx;
    audio_io_service_v2_probe_feed_rx_frame(frame->samples, frame->sample_count);
}

static void rx_dispatch_session_mirror_cb(const nb_audio_io_v2_pcm_frame_t *frame,
                                          void *ctx)
{
    const rx_dispatch_context_t *dispatch = (const rx_dispatch_context_t *)ctx;
    if (dispatch != NULL && dispatch->io_v2_session_context) {
        audio_io_service_v2_session_rx_mirror_feed(frame->samples,
                                                   frame->sample_count);
    }
}

static void rx_dispatch_activity_cb(const nb_audio_io_v2_pcm_frame_t *frame,
                                    void *ctx)
{
    const rx_dispatch_context_t *dispatch = (const rx_dispatch_context_t *)ctx;
    if (dispatch == NULL) {
        return;
    }
    voice_activity_service_v2_feed_frame(frame->samples,
                                         frame->sample_count,
                                         dispatch->activity_session_context,
                                         dispatch->activity_playback_context);
}

static void rx_dispatch_vad_cb(const nb_audio_io_v2_pcm_frame_t *frame,
                               void *ctx)
{
    const rx_dispatch_context_t *dispatch = (const rx_dispatch_context_t *)ctx;
    if (dispatch == NULL || dispatch->mic_samples == NULL) {
        return;
    }
    vad_update(dispatch->mic_samples,
               frame->samples,
               frame->sample_count,
               dispatch->local_output_active);
}

static void rx_dispatch_preroll_cb(const nb_audio_io_v2_pcm_frame_t *frame,
                                   void *ctx)
{
    const rx_dispatch_context_t *dispatch = (const rx_dispatch_context_t *)ctx;
    if (dispatch == NULL || dispatch->wake_samples == NULL) {
        return;
    }
    if (!dispatch->io_v2_session_context &&
        (!dispatch->local_output_active || dispatch->activity_bridge_say_context)) {
        wake_service_feed(dispatch->wake_samples, frame->sample_count);
    }
    if (frame->sample_count > 0U) {
        memcpy(s_preroll_buf[s_preroll_head],
               frame->samples,
               (size_t)frame->sample_count * sizeof(int16_t));
        s_preroll_head = (uint8_t)((s_preroll_head + 1U) % PREROLL_CHUNKS);
        if (s_preroll_count < PREROLL_CHUNKS) {
            s_preroll_count++;
        }
    }
}

static bool listen_start_bridge_capture(void)
{
    if (s.bridge_start_sent) return true;
    if (!s.listen_session_active) return false;

    if (!bridge_service_is_connected()) {
        ESP_LOGI(TAG, "captura local sem bridge conectada");
        return false;
    }

    esp_err_t start_rc = s.listen_capture_v2_tx_owner
        ? voice_capture_session_v2_send_voice_start()
        : ESP_OK;
    if (!s.listen_capture_v2_tx_owner) {
        bridge_service_flush_tx();
        nb_event_t marker = {
            .type         = NB_EVT_VOICE_ACTIVITY_START,
            .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
        };
        start_rc = bridge_service_send_event(&marker);
    }
    s.bridge_start_sent = (start_rc == ESP_OK);
    if (!s.bridge_start_sent) {
        ESP_LOGW(TAG, "VOICE_START nao entrou na fila da bridge: %s",
                 esp_err_to_name(start_rc));
        return false;
    }

    audio_processor_service_bridge_capture_begin();

    uint8_t pr_count = s.listen_skip_preroll ? 0U : s_preroll_count;
    if (pr_count > 0U) {
        uint8_t start = (uint8_t)((s_preroll_head + PREROLL_CHUNKS - pr_count) % PREROLL_CHUNKS);
        for (uint8_t i = 0; i < pr_count; i++) {
            uint8_t idx = (uint8_t)((start + i) % PREROLL_CHUNKS);
            int32_t raw_rms = 0;
            int32_t tx_rms = 0;
            uint16_t raw_peak = 0;
            uint16_t tx_peak = 0;
            uint16_t saturated = 0;
            bridge_prepare_tx_audio(s_preroll_buf[idx], s_bridge_buf,
                                    NB_AUDIO_CHUNK_FRAMES,
                                    &raw_rms, &raw_peak, &tx_rms, &tx_peak, &saturated);
            if (bridge_service_opus_is_enabled()) {
                bridge_feed_opus_pcm(s_bridge_buf, NB_AUDIO_CHUNK_FRAMES);
                uint8_t sent_packets = bridge_drain_opus_packets_if_enabled(
                    s.listen_capture_v2_tx_owner);
                if (sent_packets > 0U) {
                    s.bridge_audio_sent = true;
                    if (s.listen_capture_v2_active && !s.listen_capture_v2_tx_owner) {
                        voice_capture_session_v2_note_audio_chunk(
                            (uint16_t)sent_packets * NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES,
                            true);
                    }
                }
            } else {
                esp_err_t tx_rc = s.listen_capture_v2_tx_owner
                    ? voice_capture_session_v2_send_audio_chunk(s_bridge_buf,
                                                                NB_AUDIO_CHUNK_FRAMES)
                    : bridge_service_send_audio_chunk(s_bridge_buf,
                                                      NB_AUDIO_CHUNK_FRAMES);
                if (tx_rc == ESP_OK) {
                    bridge_feed_opus_pcm(s_bridge_buf, NB_AUDIO_CHUNK_FRAMES);
                    s.bridge_audio_sent = true;
                    if (s.listen_capture_v2_active && !s.listen_capture_v2_tx_owner) {
                        voice_capture_session_v2_note_audio_chunk(NB_AUDIO_CHUNK_FRAMES, true);
                    }
                } else if (s.listen_capture_v2_active && !s.listen_capture_v2_tx_owner) {
                    voice_capture_session_v2_note_audio_chunk(NB_AUDIO_CHUNK_FRAMES, false);
                }
            }
        }
    } else if (s.listen_skip_preroll) {
        ESP_LOGI(TAG, "pre-roll suprimido para barge-in");
    }

    s.bridge_tx_active = true;
    if (s.listen_capture_v2_active && !s.listen_capture_v2_tx_owner) {
        voice_capture_session_v2_note_voice_start();
    }
    ESP_LOGD(TAG, "bridge captura iniciada preroll=%u", (unsigned)pr_count);
    return true;
}

static nb_voice_capture_v2_source_t capture_v2_source_from_listen(nb_listen_source_t source)
{
    switch (source) {
        case NB_LISTEN_SOURCE_WAKE_WORD:
            return NB_VOICE_CAPTURE_V2_SOURCE_WAKE_WORD;
        case NB_LISTEN_SOURCE_FOLLOWUP:
            return NB_VOICE_CAPTURE_V2_SOURCE_FOLLOWUP;
        case NB_LISTEN_SOURCE_BARGE_IN:
            return NB_VOICE_CAPTURE_V2_SOURCE_BARGE_IN;
        default:
            return NB_VOICE_CAPTURE_V2_SOURCE_DEBUG;
    }
}

static bool begin_capture_v2_if_enabled(nb_listen_source_t source)
{
    s.listen_capture_v2_tx_owner = false;
    if (!config_get_voice_audio_v2_capture_enabled()) {
        return false;
    }

    esp_err_t err = voice_capture_session_v2_begin_real_pcm16(
        capture_v2_source_from_listen(source));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "capture v2 real iniciado source=%s",
                 listen_source_name(source));
        if (config_get_voice_audio_v2_capture_tx_enabled()) {
            esp_err_t owner_rc = voice_capture_session_v2_set_bridge_tx_owner(true);
            if (owner_rc == ESP_OK) {
                s.listen_capture_v2_tx_owner = true;
                ESP_LOGI(TAG, "capture v2 bridge TX owner armado source=%s",
                         listen_source_name(source));
            } else {
                ESP_LOGW(TAG, "capture v2 TX owner indisponivel (%s); mantendo TX legado",
                         esp_err_to_name(owner_rc));
            }
        }
    } else {
        ESP_LOGW(TAG, "capture v2 real indisponivel (%s); usando v1 source=%s",
                 esp_err_to_name(err), listen_source_name(source));
    }
    return err == ESP_OK;
}

static esp_err_t listen_session_finish(nb_listen_end_reason_t reason)
{
    if (!s.listen_session_active) return ESP_ERR_INVALID_STATE;

    bool can_end = s.bridge_start_sent
                && (s.bridge_audio_sent || reason == NB_LISTEN_END_CANCELLED);
    listen_phase_t phase = s.listen_phase;
    uint32_t speech_elapsed_ms = s.listen_speech_elapsed_ms;
    uint32_t legacy_tx_frames = s.listen_legacy_tx_frames;
    uint32_t legacy_tx_samples = s.listen_legacy_tx_samples;
    bool capture_v2_active = s.listen_capture_v2_active;
    bool capture_v2_tx_owner = s.listen_capture_v2_tx_owner;
    voice_activity_service_v2_session_compare_legacy_end((uint32_t)reason,
                                                          speech_elapsed_ms);
    audio_io_service_v2_session_rx_mirror_finish((uint32_t)reason,
                                                 legacy_tx_frames,
                                                 legacy_tx_samples,
                                                 speech_elapsed_ms);

    s.listen_session_active    = false;
    s.listen_phase             = LISTEN_PHASE_IDLE;
    s.listen_mode              = NB_LISTEN_MODE_AUTO;
    s.listen_wait_remaining_ms = 0;
    s.listen_speech_elapsed_ms = 0;
    s.listen_legacy_tx_frames  = 0;
    s.listen_legacy_tx_samples = 0;
    s.bridge_tx_active         = false;
    s.vad_state                = VAD_SILENCE;
    s.vad_enter_count          = 0;
    s.vad_silence_start_us     = 0;
    audio_processor_service_bridge_capture_end();
    if (capture_v2_active && !capture_v2_tx_owner) {
        voice_capture_session_v2_finish(reason == NB_LISTEN_END_CANCELLED);
    }
    s.listen_capture_v2_active = false;
    s.listen_capture_v2_tx_owner = false;

    if (s.event_cb) s.event_cb(NB_AUDIO_EVT_VOICE_END,
                               NB_AUDIO_EVT_DATA_SESSION);

    if (can_end && bridge_service_is_connected()) {
        bool flush_before_end = s.bridge_flush_before_end ||
                                reason == NB_LISTEN_END_CANCELLED;
        if (flush_before_end && !capture_v2_tx_owner) {
            bridge_service_flush_tx();
        }
        esp_err_t end_rc = capture_v2_tx_owner
            ? voice_capture_session_v2_send_voice_end((uint32_t)reason,
                                                       reason == NB_LISTEN_END_CANCELLED,
                                                       flush_before_end)
            : ESP_OK;
        if (!capture_v2_tx_owner) {
            nb_event_t marker = {
                .type         = NB_EVT_VOICE_ACTIVITY_END,
                .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
                .data.u32     = (uint32_t)reason,
            };
            end_rc = bridge_service_send_event(&marker);
        }
        if (end_rc != ESP_OK) {
            ESP_LOGW(TAG, "VOICE_END nao entrou na fila da bridge: %s",
                     esp_err_to_name(end_rc));
        } else {
            ESP_LOGI(TAG, "[ AUDIO ENVIADO — aguardando resposta ]");
        }
    } else {
        ESP_LOGI(TAG,
                 "[ FIM DE SESSAO — reason=%s phase=%s start=%d audio=%d conn=%d ]",
                 listen_end_reason_name(reason), listen_phase_name(phase),
                 (int)s.bridge_start_sent, (int)s.bridge_audio_sent,
                 (int)bridge_service_is_connected());
        if (capture_v2_active && capture_v2_tx_owner) {
            voice_capture_session_v2_finish(reason == NB_LISTEN_END_CANCELLED);
        }
    }

    ESP_LOGI(TAG, "sessao listen encerrada reason=%s phase=%s speech_ms=%u can_end=%d",
             listen_end_reason_name(reason), listen_phase_name(phase),
             (unsigned)speech_elapsed_ms, (int)can_end);

    s.bridge_start_sent = false;
    s.bridge_audio_sent = false;
    s.bridge_tx_fail_count = 0;
    s.bridge_flush_before_end = false;
    s.listen_voice_detected = false;
    s_bridge_diag_chunk_counter = 0;
    esp_vad_reset();
    return ESP_OK;
}

static void rx_dispatch_bridge_tx_cb(const nb_audio_io_v2_pcm_frame_t *frame,
                                     void *ctx)
{
    (void)ctx;
    if (!s.listen_session_active || !s.bridge_tx_active || frame->sample_count == 0U) {
        return;
    }

    int32_t raw_rms = 0;
    int32_t tx_rms = 0;
    uint16_t raw_peak = 0;
    uint16_t tx_peak = 0;
    uint16_t saturated = 0;
    bool used_processed = audio_processor_service_read_bridge_processed(
        s_bridge_proc_buf, frame->sample_count);
    const int16_t *bridge_src = used_processed ? s_bridge_proc_buf : frame->samples;
    bridge_prepare_tx_audio(bridge_src, s_bridge_buf, frame->sample_count,
                            &raw_rms, &raw_peak, &tx_rms, &tx_peak, &saturated);
    if (bridge_service_opus_is_enabled()) {
        bridge_feed_opus_pcm(s_bridge_buf, frame->sample_count);
        uint8_t sent_packets = bridge_drain_opus_packets_if_enabled(
            s.listen_capture_v2_tx_owner);
        if (sent_packets > 0U) {
            s.bridge_audio_sent = true;
            s.listen_legacy_tx_frames += sent_packets;
            s.listen_legacy_tx_samples +=
                (uint32_t)sent_packets * NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES;
            if (s.listen_capture_v2_active && !s.listen_capture_v2_tx_owner) {
                voice_capture_session_v2_note_audio_chunk(
                    (uint16_t)sent_packets * NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES,
                    true);
            }
            s.bridge_tx_fail_count = 0;
            if ((++s_bridge_diag_chunk_counter % BRIDGE_TX_LOG_CHUNKS) == 1U) {
                ESP_LOGI(TAG,
                         "bridge tx diag: source=opus raw_rms=%ld raw_peak=%u tx_rms=%ld tx_peak=%u sat=%u/%u",
                         (long)raw_rms,
                         (unsigned)raw_peak,
                         (long)tx_rms,
                         (unsigned)tx_peak,
                         (unsigned)saturated,
                         (unsigned)frame->sample_count);
            }
        }
    } else {
        esp_err_t tx_rc = s.listen_capture_v2_tx_owner
            ? voice_capture_session_v2_send_audio_chunk(s_bridge_buf, frame->sample_count)
            : bridge_service_send_audio_chunk(s_bridge_buf, frame->sample_count);
        if (tx_rc == ESP_OK) {
            bridge_feed_opus_pcm(s_bridge_buf, frame->sample_count);
            s.bridge_audio_sent = true;
            s.listen_legacy_tx_frames++;
            s.listen_legacy_tx_samples += (uint32_t)frame->sample_count;
            if (s.listen_capture_v2_active && !s.listen_capture_v2_tx_owner) {
                voice_capture_session_v2_note_audio_chunk(frame->sample_count, true);
            }
            s.bridge_tx_fail_count = 0;
            if ((++s_bridge_diag_chunk_counter % BRIDGE_TX_LOG_CHUNKS) == 1U) {
                ESP_LOGI(TAG,
                         "bridge tx diag: source=%s raw_rms=%ld raw_peak=%u tx_rms=%ld tx_peak=%u sat=%u/%u",
                         used_processed ? "afe" : "raw",
                         (long)raw_rms,
                         (unsigned)raw_peak,
                         (long)tx_rms,
                         (unsigned)tx_peak,
                         (unsigned)saturated,
                         (unsigned)frame->sample_count);
            }
        } else {
            if (s.listen_capture_v2_active && !s.listen_capture_v2_tx_owner) {
                voice_capture_session_v2_note_audio_chunk(frame->sample_count, false);
            }
            if (tx_rc == ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "bridge desconectou durante sessao — encerrando escuta");
                s.bridge_flush_before_end = false;
                listen_session_finish(NB_LISTEN_END_BRIDGE_DISCONNECTED);
            } else if (tx_rc == ESP_ERR_NO_MEM) {
                /* Backpressure persistente não é fala saudável: a fila já ficou
                 * atrás do tempo real. Encerra limpo após poucas falhas para não
                 * alimentar STT com áudio picotado ou atrasado. */
                if (s.bridge_tx_fail_count < UINT8_MAX) {
                    s.bridge_tx_fail_count++;
                }
                if (s.bridge_tx_fail_count >= BRIDGE_TX_FAIL_ABORT_COUNT) {
                    ESP_LOGW(TAG,
                             "bridge TX congestionado (%u chunks dropados seguidos) — encerrando sessao",
                             (unsigned)s.bridge_tx_fail_count);
                    s.bridge_flush_before_end = true;
                    listen_session_finish(NB_LISTEN_END_TIMEOUT);
                }
            } else {
                if (s.bridge_tx_fail_count < UINT8_MAX) {
                    s.bridge_tx_fail_count++;
                }
                if (s.bridge_tx_fail_count >= BRIDGE_TX_FAIL_ABORT_COUNT) {
                    ESP_LOGW(TAG,
                             "bridge TX travou (%u falhas seguidas: %s) — encerrando sessao",
                             (unsigned)s.bridge_tx_fail_count, esp_err_to_name(tx_rc));
                    s.bridge_flush_before_end = true;
                    listen_session_finish(NB_LISTEN_END_BRIDGE_DISCONNECTED);
                }
            }
        }
    }
}

static void vad_update(const int32_t *mic, const int16_t *pcm16, size_t n,
                       bool local_output_active)
{
    if (n == 0) return;

    int64_t sum_sq = 0;
    uint32_t zcr_count = 0;
    for (size_t i = 0; i < n; i++) {
        int64_t v = mic[i];
        sum_sq += v * v;
        if (i > 0 && ((mic[i - 1] >= 0) != (mic[i] >= 0))) zcr_count++;
    }
    int32_t rms = (int32_t)sqrtf((float)(sum_sq / (int64_t)n));

    /* ZCR: taxa de cruzamentos de zero. Faixa estreita para fala próxima:
     * carros/vento tendem a baixo ZCR; chiado/TV/música tendem a variar alto. */
    float zcr = (n > 1u) ? ((float)zcr_count / (float)(n - 1u)) : 0.0f;

    if (local_output_active) {
        s.vad_playback_mute_ms = VAD_PLAYBACK_MUTE_MS;
    } else if (s.vad_playback_mute_ms > 0U) {
        s.vad_playback_mute_ms = (s.vad_playback_mute_ms > CHUNK_DURATION_MS)
                               ? s.vad_playback_mute_ms - CHUNK_DURATION_MS : 0U;
    }

    /* Settling pós-init: calibra ruído, mas não emite eventos. */
    if (s.vad_settle_ms > 0U) {
        s.vad_noise_ema = s.vad_noise_ema * 0.98f + (float)rms * 0.02f;
        s.vad_threshold = (int32_t)fmaxf(VAD_MIN_RMS_ABS,
                                         s.vad_noise_ema * VAD_NOISE_MULT_ENTER);
        s.vad_settle_ms = (s.vad_settle_ms > CHUNK_DURATION_MS)
                          ? s.vad_settle_ms - CHUNK_DURATION_MS : 0U;
        if (s.vad_settle_ms == 0U) {
            ESP_LOGI(TAG, "VAD settling completo — noise_ema=%ld thr=%ld",
                     (long)s.vad_noise_ema, (long)s.vad_threshold);
        }
        return;
    }

    float voice_ratio = sound_analysis_get_voice_ratio();
    float voice_low_ratio = sound_analysis_get_voice_low_ratio();
    float voice_mid_ratio = sound_analysis_get_voice_mid_ratio();
    float high_ratio  = sound_analysis_get_high_ratio();
    float low_ratio   = sound_analysis_get_low_ratio();
    float dom_freq    = sound_analysis_get_dominant_freq();
    nb_sound_class_t cls = sound_analysis_get_class();

    bool listen_profile = s.listen_session_active;
    float min_rms_abs = listen_profile ? VAD_LISTEN_MIN_RMS_ABS : VAD_MIN_RMS_ABS;
    float min_rms_soft = listen_profile ? VAD_LISTEN_MIN_RMS_SOFT : VAD_MIN_RMS_SOFT;
    float enter_mult = listen_profile ? VAD_LISTEN_NOISE_MULT_ENTER : VAD_NOISE_MULT_ENTER;
    float soft_mult = listen_profile ? VAD_LISTEN_NOISE_MULT_SOFT : VAD_NOISE_MULT_SOFT;
    float active_mult = listen_profile ? VAD_LISTEN_NOISE_MULT_ACTIVE : VAD_NOISE_MULT_ACTIVE;
    float enter_thr = fmaxf(min_rms_abs, s.vad_noise_ema * enter_mult);
    float active_thr = fmaxf(min_rms_abs * 0.75f,
                             s.vad_noise_ema * active_mult);
    float soft_thr = fmaxf(min_rms_soft, s.vad_noise_ema * soft_mult);
    s.vad_threshold = (int32_t)((s.vad_state == VAD_ACTIVE) ? active_thr : enter_thr);

    bool zcr_ok      = (zcr >= VAD_ZCR_MIN) && (zcr <= VAD_ZCR_MAX);
    bool formant_ok  = (voice_mid_ratio >= VAD_VOICE_MID_RATIO_MIN);
    bool low_tonal_engine = (dom_freq >= VAD_DOM_FREQ_MIN_HZ)
                         && (dom_freq <= VAD_ENGINE_DOM_MAX_HZ)
                         && (voice_low_ratio > (voice_mid_ratio * VAD_ENGINE_LOW_DOMINANCE))
                         && (high_ratio <= VAD_ENGINE_HIGH_RATIO_MAX)
                         && (zcr <= VAD_ENGINE_ZCR_MAX);
    bool engine_like = ((float)rms >= VAD_ENGINE_RMS_MIN) && low_tonal_engine;
    bool spectral_ok = (cls == NB_SOUND_VOICE || voice_ratio >= VAD_VOICE_RATIO_MIN)
                    && formant_ok
                    && !engine_like
                    && (high_ratio <= VAD_HIGH_RATIO_MAX)
                    && (low_ratio <= VAD_LOW_RATIO_MAX)
                    && (dom_freq >= VAD_DOM_FREQ_MIN_HZ);
    bool muted       = (s.vad_playback_mute_ms > 0U);
    bool energy_ok   = (float)rms >= (float)s.vad_threshold;
    bool soft_energy_ok = (float)rms >= soft_thr;
    bool heuristic_speech_strong = energy_ok && zcr_ok && spectral_ok && !muted;
    bool heuristic_speech = soft_energy_ok && zcr_ok && spectral_ok && !muted;

    int esp_vad_state = esp_vad_update(pcm16, n, muted);
    bool esp_vad_speech = (esp_vad_state == ESP_SR_VAD_SPEECH);
    bool speech_strong = esp_vad_speech;
    bool is_speech = esp_vad_speech;
    bool listen_heuristic = s.listen_session_active && heuristic_speech;
    if (listen_heuristic) {
        is_speech = true;
        speech_strong = speech_strong || heuristic_speech_strong;
    }
#if NB_AUDIO_HEURISTIC_LISTEN_FALLBACK
    if (!s.esp_vad_enabled) {
        speech_strong = heuristic_speech_strong;
        is_speech = heuristic_speech;
    }
#endif

    int64_t now_us = esp_timer_get_time();

    if (is_speech
            && s.listen_session_active
            && s.listen_voice_detected
            && s.listen_phase == LISTEN_PHASE_ENDING_ON_SILENCE
            && !speech_strong) {
        if (s.vad_silence_start_us == 0) {
            s.vad_silence_start_us = now_us;
        } else {
            int64_t silence_ms = (now_us - s.vad_silence_start_us) / 1000LL;
            if (silence_ms >= (int64_t)listen_current_end_silence_ms()) {
                s.vad_state        = VAD_SILENCE;
                s.bridge_tx_active = false;
                listen_session_finish(NB_LISTEN_END_VAD_SILENCE);
            }
        }
        return;
    }

    if (is_speech) {
        if (s.vad_state == VAD_SILENCE) {
            uint8_t inc = speech_strong ? VAD_ENTER_SCORE_STRONG_INC
                                        : VAD_ENTER_SCORE_SOFT_INC;
            uint16_t next_score = (uint16_t)s.vad_enter_count + inc;
            s.vad_enter_count = (next_score > VAD_ENTER_SCORE_START)
                              ? VAD_ENTER_SCORE_START : (uint8_t)next_score;
            if (s.vad_enter_count >= VAD_ENTER_SCORE_START) {
                s.vad_enter_count = 0;
                s.vad_state = VAD_ACTIVE;
                s.vad_silence_start_us = 0;
                /* 12.11: VAD nao abre sessao — sessao abre por wake word.
                 * O callback so vira evento publico quando ha sessao ativa. */
                if (s.event_cb) {
                    s.event_cb(NB_AUDIO_EVT_VOICE_START,
                               s.listen_session_active ? NB_AUDIO_EVT_DATA_SESSION : 0U);
                }
                if (s.listen_session_active) {
                    s.listen_voice_detected = true;
                    s.listen_phase = LISTEN_PHASE_CAPTURING_SPEECH;
                    s.listen_wait_remaining_ms = 0;
                    s.listen_speech_elapsed_ms = 0;
                    if (listen_start_bridge_capture()) {
                        ESP_LOGI(TAG, "[ VOZ DETECTADA — capturando... ]");
                    } else {
                        listen_session_finish(NB_LISTEN_END_BRIDGE_DISCONNECTED);
                    }
                }
            }
        } else {
            s.vad_enter_count = 0;
            s.vad_silence_start_us = 0;  /* reinicia contador de silêncio */
            if (s.listen_session_active) {
                if (!s.listen_voice_detected) {
                    s.listen_voice_detected = true;
                    s.listen_wait_remaining_ms = 0;
                    s.listen_speech_elapsed_ms = 0;
                    if (listen_start_bridge_capture()) {
                        ESP_LOGI(TAG, "[ VOZ DETECTADA — capturando... ]");
                    } else {
                        listen_session_finish(NB_LISTEN_END_BRIDGE_DISCONNECTED);
                    }
                }
                s.listen_phase = LISTEN_PHASE_CAPTURING_SPEECH;
            }
        }
    } else {
        if (s.vad_enter_count > VAD_ENTER_SCORE_DEC) {
            s.vad_enter_count -= VAD_ENTER_SCORE_DEC;
        } else {
            s.vad_enter_count = 0;
        }
        /* RMS alto mas ZCR fora da faixa de fala (ruído, fan, música) — ignora. */
        /* Log periódico do pico de ruído ambiente para calibração. */
        if (s.vad_state == VAD_SILENCE) {
            /* Atualiza EMA apenas em ruído não-rumble ou quando o nível caiu.
             * Motor grave não deve subir o threshold, senão a fala morre. */
            bool mechanical_noise = (low_ratio > VAD_LOW_RATIO_MAX)
                                 || (dom_freq < VAD_DOM_FREQ_MIN_HZ)
                                 || low_tonal_engine
                                 || (((float)rms > enter_thr)
                                     && !formant_ok
                                     && (high_ratio < 0.06f));
            if (!muted && (!mechanical_noise || (float)rms < s.vad_noise_ema)) {
                s.vad_noise_ema = s.vad_noise_ema * 0.995f + (float)rms * 0.005f;
            }
            s.vad_threshold = (int32_t)fmaxf(VAD_MIN_RMS_ABS,
                                             s.vad_noise_ema * VAD_NOISE_MULT_ENTER);

            if (rms > s.vad_noise_peak) s.vad_noise_peak = rms;
        }
        if (s.vad_state == VAD_ACTIVE) {
            if (s.vad_silence_start_us == 0) {
                s.vad_silence_start_us = now_us;
                if (s.listen_session_active && s.listen_voice_detected) {
                    s.listen_phase = LISTEN_PHASE_ENDING_ON_SILENCE;
                }
            } else {
                int64_t silence_ms = (now_us - s.vad_silence_start_us) / 1000LL;
                uint32_t end_silence_ms = (s.listen_session_active && s.listen_voice_detected)
                                        ? listen_current_end_silence_ms()
                                        : NB_AUDIO_VAD_SILENCE_MS;
                if (silence_ms >= (int64_t)end_silence_ms) {
                    s.vad_state        = VAD_SILENCE;
                    s.bridge_tx_active = false;
                    if (s.listen_session_active) {
                        listen_session_finish(NB_LISTEN_END_VAD_SILENCE);
                    } else if (s.event_cb) {
                        s.event_cb(NB_AUDIO_EVT_VOICE_END, 0U);
                    }
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
            (void)audio_playback_service_v2_say_cancel();
            (void)audio_hal_spk_write_silence(NB_AUDIO_CHUNK_FRAMES, 0);
            if (s.event_cb) s.event_cb(NB_AUDIO_EVT_PLAYBACK_END, 0);
            xSemaphoreTake(s.mutex, portMAX_DELAY);
            s.play_state = PLAY_IDLE;
            s.bridge_say_playing = false;
            s.bridge_say_empty_ms = 0;
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
                    esp_err_t wr = audio_hal_spk_write(s_wav_chunk, WAV_SAMPLES_PER_CHUNK,
                                                       pdMS_TO_TICKS(100));
                    audio_note_spk_result(wr, "wav", WAV_SAMPLES_PER_CHUNK, false);
                    wrote_audio = true;
                }
            }
        }

        /* ── Bridge SAY playback ─────────────────────────────────────────── */
        else if (play_state == PLAY_BRIDGE_SAY) {
            uint16_t n = 0;
            esp_err_t wr = ESP_OK;
            if (audio_playback_service_v2_speaker_write_next_frame(
                    s.volume,
                    audio_service_playback_v2_write_speaker,
                    NULL,
                    &n,
                    &wr)) {
                s.bridge_say_empty_ms = 0;
                audio_note_spk_result(wr, "bridge_say", n, false);
                if ((++s_bridge_say_play_count % 64U) == 1U) {
                    ESP_LOGI(TAG, "Bridge SAY playback chunks=%lu q=%u",
                             (unsigned long)s_bridge_say_play_count,
                             (unsigned)audio_playback_service_v2_say_pending_count());
                }
                wrote_audio = true;
            } else {
                /* Fila vazia pode ser jitter de TCP/TTS. Segura o modo SAY por
                 * uma janela curta antes de encerrar para não cortar frases. */
                if (audio_playback_service_v2_speaker_note_empty(CHUNK_DURATION_MS,
                                                                 BRIDGE_SAY_IDLE_END_MS)) {
                    xSemaphoreTake(s.mutex, portMAX_DELAY);
                    s.play_state      = PLAY_IDLE;
                    s.bridge_say_playing = false;
                    s.bridge_say_empty_ms = 0;
                    xSemaphoreGive(s.mutex);
                    audio_playback_service_v2_note_say_idle();
                    if (s.event_cb) s.event_cb(NB_AUDIO_EVT_PLAYBACK_END, 0);
                }
            }
        }

        /* Synth — só ativo quando não há WAV reproduzindo */
        if (!wrote_audio) {
            if (audio_playback_service_v2_fill_probe_chunk(s_wav_chunk,
                                                           NB_AUDIO_CHUNK_FRAMES)) {
                esp_err_t wr = audio_hal_spk_write(s_wav_chunk, NB_AUDIO_CHUNK_FRAMES,
                                                   pdMS_TO_TICKS(100));
                audio_note_spk_result(wr, "playback_v2_probe",
                                      NB_AUDIO_CHUNK_FRAMES, false);
                wrote_audio = true;
            } else if (synth_fill_chunk(s_wav_chunk, NB_AUDIO_CHUNK_FRAMES)) {
                esp_err_t wr = audio_hal_spk_write(s_wav_chunk, NB_AUDIO_CHUNK_FRAMES,
                                                   pdMS_TO_TICKS(100));
                audio_note_spk_result(wr, "synth", NB_AUDIO_CHUNK_FRAMES, false);
                wrote_audio = true;
            } else {
                esp_err_t wr = audio_hal_spk_write_silence(NB_AUDIO_CHUNK_FRAMES,
                                                           pdMS_TO_TICKS(100));
                audio_note_spk_result(wr, "silence", NB_AUDIO_CHUNK_FRAMES, true);
            }
        }

        /* ── 2. Chunk RX ─────────────────────────────────────────────────── */
        size_t mic_n = 0;
        esp_err_t rc = audio_hal_mic_read(s_mic_buf, NB_AUDIO_CHUNK_FRAMES,
                                           &mic_n, pdMS_TO_TICKS(100));
        if (rc != ESP_OK) {
            audio_note_mic_result(rc);
            continue;
        }
        audio_note_mic_result(ESP_OK);

        for (size_t i = 0; i < mic_n; i++) {
            int32_t v = s_mic_buf[i] >> 8;
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            s_wake_buf[i] = (int16_t)v;
        }

        mic_condition_signal(s_mic_buf, s_mic_proc, mic_n);

        /* ── 3. Sound analysis ──────────────────────────────────────────── */
        /* Converte int32 (24-bit) → int16 sem ganho extra (nível calibrado). */
        for (size_t i = 0; i < mic_n; i++) {
            int32_t v = s_mic_proc[i] >> 8;
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            s_sa_buf[i] = (int16_t)v;
        }
        rx_dispatch_context_t rx_dispatch = {0};
        xSemaphoreTake(s.mutex, portMAX_DELAY);
        rx_dispatch.io_v2_session_context = s.listen_session_active;
        rx_dispatch.activity_session_context = s.listen_session_active;
        rx_dispatch.activity_bridge_say_context = s.bridge_say_playing;
        xSemaphoreGive(s.mutex);
        rx_dispatch.local_output_active = wrote_audio;
        rx_dispatch.mic_samples = s_mic_proc;
        rx_dispatch.wake_samples = s_wake_buf;
        rx_dispatch.activity_playback_context =
            wrote_audio ||
            play_state == PLAY_ACTIVE ||
            play_state == PLAY_BRIDGE_SAY ||
            rx_dispatch.activity_bridge_say_context ||
            audio_playback_service_v2_is_playing();
        const nb_audio_io_v2_rx_consumer_t rx_consumers[] = {
            { .cb = rx_dispatch_sound_analysis_cb, .ctx = NULL },
            { .cb = rx_dispatch_processor_shadow_cb, .ctx = NULL },
            { .cb = rx_dispatch_io_probe_cb, .ctx = NULL },
            { .cb = rx_dispatch_session_mirror_cb, .ctx = &rx_dispatch },
            { .cb = rx_dispatch_activity_cb, .ctx = &rx_dispatch },
            { .cb = rx_dispatch_vad_cb, .ctx = &rx_dispatch },
            { .cb = rx_dispatch_preroll_cb, .ctx = &rx_dispatch },
            { .cb = rx_dispatch_bridge_tx_cb, .ctx = NULL },
        };
        audio_io_service_v2_rx_dispatch_frame(s_sa_buf,
                                              (uint16_t)mic_n,
                                              0U,
                                              rx_consumers,
                                              (uint8_t)(sizeof(rx_consumers) /
                                                        sizeof(rx_consumers[0])),
                                              NULL);

        /* ── 4d. Session timeouts ────────────────────────────────────────── */
        if (s.listen_session_active) {
            if (s.listen_phase == LISTEN_PHASE_WAITING_FOR_SPEECH) {
                if (s.listen_wait_remaining_ms <= CHUNK_DURATION_MS) {
                    listen_session_finish(NB_LISTEN_END_TIMEOUT);
                } else {
                    s.listen_wait_remaining_ms -= CHUNK_DURATION_MS;
                }
            } else if (s.listen_phase == LISTEN_PHASE_CAPTURING_SPEECH
                    || s.listen_phase == LISTEN_PHASE_ENDING_ON_SILENCE) {
                if (s.listen_speech_elapsed_ms >= LISTEN_MAX_SPEECH_MS) {
                    ESP_LOGW(TAG, "sessao listen atingiu teto de fala (%ums)",
                             (unsigned)LISTEN_MAX_SPEECH_MS);
                    listen_session_finish(NB_LISTEN_END_TIMEOUT);
                } else {
                    s.listen_speech_elapsed_ms += CHUNK_DURATION_MS;
                }
            }
        }

        /* ── 5. Diagnóstico ─────────────────────────────────────────────── */
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
                if (s.rec_bridge_tx_source) {
                    bridge_prepare_tx_audio(s_sa_buf, s_rec_chunk, to_write,
                                            NULL, NULL, NULL, NULL, NULL);
                } else {
                    for (size_t i = 0; i < to_write; i++) {
                        /*
                         * s_mic_proc[i]: valor 24-bit condicionado (audio_hal já
                         * fez >> 8 do raw 32-bit).
                         * Shift >> 8 para descer ao range 16-bit antes do clamp.
                         * Pico típico de voz direta ~400K–800K → ~1500–3000 em 16-bit.
                         */
                        int32_t v = (s_mic_proc[i] >> 8) << 3;  /* +18 dB de ganho de gravação */
                        if (v >  32767) v =  32767;
                        if (v < -32768) v = -32768;
                        s_rec_chunk[i] = (int16_t)v;
                    }
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

    err = audio_playback_service_v2_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "audio_playback_service_v2_init falhou: %s", esp_err_to_name(err));
        audio_hal_deinit();
        return err;
    }

    err = voice_activity_service_v2_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "voice_activity_service_v2_init falhou: %s", esp_err_to_name(err));
        (void)audio_playback_service_v2_deinit();
        audio_hal_deinit();
        return err;
    }

    s.mutex = xSemaphoreCreateMutex();
    if (!s.mutex) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex falhou");
        (void)voice_activity_service_v2_deinit();
        (void)audio_playback_service_v2_deinit();
        audio_hal_deinit();
        return ESP_ERR_NO_MEM;
    }

    s.volume         = 80U;  /* Default; boot_manager ajusta via audio_set_volume */
    s.vad_threshold  = NB_AUDIO_VAD_THRESHOLD_DEFAULT;
    s.vad_noise_ema  = (float)NB_AUDIO_VAD_THRESHOLD_DEFAULT;
    s.vad_state      = VAD_SILENCE;
    s.vad_settle_ms  = NB_AUDIO_VAD_SETTLE_MS;
    s.esp_vad = vad_create_with_param(ESP_SR_VAD_MODE, 16000,
                                      ESP_SR_VAD_FRAME_MS,
                                      ESP_SR_VAD_MIN_SPEECH_MS,
                                      ESP_SR_VAD_MIN_NOISE_MS);
    s.esp_vad_enabled = (s.esp_vad != NULL);
    s.esp_vad_pos = 0;
    s.esp_vad_last_state = 0;
    if (!s.esp_vad_enabled) {
        ESP_LOGW(TAG, "ESP-SR VAD indisponivel — listening LLM sem VAD primario");
#if NB_AUDIO_HEURISTIC_LISTEN_FALLBACK
        ESP_LOGW(TAG, "fallback heuristico de listening habilitado para bancada");
#endif
    }
    s.play_state     = PLAY_IDLE;
    s.rec_state      = REC_IDLE;
    s.rec_bridge_tx_source = false;
    s.bridge_tx_active            = false;
    s.bridge_say_playing          = false;
    s.bridge_say_empty_ms         = 0;
    s.listen_session_active       = false;
    s.bridge_start_sent           = false;
    s.bridge_audio_sent           = false;
    s.listen_legacy_tx_frames     = 0;
    s.listen_legacy_tx_samples    = 0;
    s.bridge_tx_fail_count        = 0;
    s.bridge_flush_before_end     = false;
    s.listen_phase                = LISTEN_PHASE_IDLE;
    s.listen_mode                 = NB_LISTEN_MODE_AUTO;
    s.listen_wait_remaining_ms    = 0;
    s.listen_speech_elapsed_ms    = 0;
    s.listen_voice_detected       = false;
    s.listen_skip_preroll         = false;
    s_bridge_say_drop_count       = 0;
    s_bridge_say_rx_count         = 0;
    s_bridge_say_play_count       = 0;

    BaseType_t rc = xTaskCreatePinnedToCore(
        audio_task, "audio_task",
        AUDIO_TASK_STACK, NULL,
        AUDIO_TASK_PRIORITY, NULL,
        AUDIO_TASK_CORE
    );
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore audio_task falhou");
        if (s.esp_vad) {
            vad_destroy(s.esp_vad);
            s.esp_vad = NULL;
            s.esp_vad_enabled = false;
        }
        (void)audio_playback_service_v2_deinit();
        (void)voice_activity_service_v2_deinit();
        vSemaphoreDelete(s.mutex);
        audio_hal_deinit();
        return ESP_ERR_NO_MEM;
    }

    s.initialized = true;
    ESP_LOGI(TAG, "inicializado (vol=%u, vad_thr=%ld, esp_vad=%d)",
             (unsigned)s.volume, (long)s.vad_threshold,
             (int)s.esp_vad_enabled);
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
    if (audio_playback_service_v2_is_playing()) {
        (void)audio_playback_service_v2_probe_stop();
    }
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    if (s.play_state == PLAY_ACTIVE ||
        s.play_state == PLAY_BRIDGE_SAY ||
        s.bridge_say_playing) {
        s.play_state = PLAY_STOP;
        s.bridge_say_playing = false;
        s.bridge_say_empty_ms = 0;
        (void)audio_playback_service_v2_say_cancel();
    }
    xSemaphoreGive(s.mutex);
    return ESP_OK;
}

bool audio_is_playing(void)
{
    return s.initialized &&
           (s.play_state == PLAY_ACTIVE ||
            s.play_state == PLAY_BRIDGE_SAY ||
            s.bridge_say_playing ||
            audio_playback_service_v2_is_playing());
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
    s.rec_bridge_tx_source = false;
    s.rec_state = REC_ACTIVE;

    ESP_LOGI(TAG, "gravacao agendada: %s (%us)", path, (unsigned)duration_s);
    return ESP_OK;
}

esp_err_t audio_record_bridge_tx_diagnostic(const char *path, uint32_t duration_s)
{
    if (!s.initialized || !path)             return ESP_ERR_INVALID_STATE;
    if (s.rec_state == REC_ACTIVE)           return ESP_ERR_INVALID_STATE;
    if (duration_s == 0 || duration_s > 10U) return ESP_ERR_INVALID_ARG;

    snprintf(s.rec_path, sizeof(s.rec_path), "%s", path);
    s.rec_samples_remaining = duration_s * 16000U;
    s.rec_bridge_tx_source = true;
    s.rec_state = REC_ACTIVE;

    ESP_LOGI(TAG, "gravacao bridge tx agendada: %s (%us)", path, (unsigned)duration_s);
    return ESP_OK;
}

/* ── Sessão de escuta (Etapa 12.3) ──────────────────────────────────────── */

esp_err_t audio_service_begin_listen_session(nb_listen_source_t source)
{
    return audio_service_begin_listen_session_with_mode(source, NB_LISTEN_MODE_AUTO);
}

esp_err_t audio_service_begin_listen_session_with_mode(nb_listen_source_t source,
                                                       nb_listen_mode_t mode)
{
    if (!s.initialized)           return ESP_ERR_INVALID_STATE;
    if (s.listen_session_active)  return ESP_ERR_INVALID_STATE;
    if (mode != NB_LISTEN_MODE_AUTO) {
        ESP_LOGI(TAG, "listen mode=%s ainda nao suportado", listen_mode_name(mode));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (source == NB_LISTEN_SOURCE_TOUCH) {
        ESP_LOGI(TAG, "listen por touch desabilitado — touch reservado para interação");
        return ESP_ERR_NOT_SUPPORTED;
    }

    (void)voice_activity_service_v2_session_compare_begin();
    (void)audio_io_service_v2_session_rx_mirror_begin((uint32_t)source);
    bool capture_v2_active = begin_capture_v2_if_enabled(source);

    s.listen_session_active       = true;
    s.listen_mode                 = mode;
    s.bridge_start_sent           = false;
    s.bridge_audio_sent           = false;
    s.bridge_tx_fail_count        = 0;
    s.bridge_flush_before_end     = false;
    s.listen_phase                = LISTEN_PHASE_WAITING_FOR_SPEECH;
    s.listen_wait_remaining_ms    = LISTEN_WAIT_SPEECH_TIMEOUT_MS;
    s.listen_speech_elapsed_ms    = 0;
    s.listen_voice_detected       = false;
    s.listen_skip_preroll         = (source == NB_LISTEN_SOURCE_BARGE_IN);
    s.listen_capture_v2_active    = capture_v2_active;
    s.bridge_tx_active            = false;  /* liga apenas quando o VAD detectar fala */
    wake_service_suspend();
    esp_vad_reset();

    /* Wake/follow-up: áudio recente pode deixar o VAD em ACTIVE antes de a sessão abrir.
     * Resetar para SILENCE evita VOICE_END prematuro por silêncio pós-wake-word. */
    if (source == NB_LISTEN_SOURCE_WAKE_WORD ||
        source == NB_LISTEN_SOURCE_FOLLOWUP ||
        source == NB_LISTEN_SOURCE_BARGE_IN) {
        s.vad_state            = VAD_SILENCE;
        s.vad_enter_count      = 0;
        s.vad_silence_start_us = 0;
    }

    if (source == NB_LISTEN_SOURCE_WAKE_WORD || source == NB_LISTEN_SOURCE_BARGE_IN) {
        if (source == NB_LISTEN_SOURCE_BARGE_IN) {
            ESP_LOGI(TAG, "[ TE OUVI — CONTINUE ]");
        } else {
            ESP_LOGI(TAG, "[ PODE FALAR ]");
        }
        if (source == NB_LISTEN_SOURCE_WAKE_WORD) {
            ESP_LOGI(TAG, "captura bridge aguardando VAD source=%s",
                     listen_source_name(source));
        } else if (listen_start_bridge_capture()) {
            s.listen_voice_detected = true;
            s.listen_phase = LISTEN_PHASE_CAPTURING_SPEECH;
            s.listen_wait_remaining_ms = 0;
            s.listen_speech_elapsed_ms = 0;
            s.vad_silence_start_us = 0;
            ESP_LOGI(TAG, "bridge captura aberta imediatamente source=%s",
                     listen_source_name(source));
        } else {
            listen_session_finish(NB_LISTEN_END_BRIDGE_DISCONNECTED);
            return ESP_ERR_INVALID_STATE;
        }
    } else if (source == NB_LISTEN_SOURCE_FOLLOWUP) {
        ESP_LOGI(TAG, "[ PODE CONTINUAR ]");
    }
    ESP_LOGI(TAG, "sessao listen aberta source=%s mode=%s phase=%s wait=%ums preroll=%u bridge_conn=%d",
             listen_source_name(source), listen_mode_name(s.listen_mode),
             listen_phase_name(s.listen_phase),
             (unsigned)s.listen_wait_remaining_ms, (unsigned)s_preroll_count,
             (int)bridge_service_is_connected());
    return ESP_OK;
}

esp_err_t audio_service_end_listen_session(nb_listen_end_reason_t reason)
{
    if (!s.initialized)           return ESP_ERR_INVALID_STATE;
    if (!s.listen_session_active) return ESP_ERR_INVALID_STATE;

    return listen_session_finish(reason);
}

bool audio_service_is_listening(void)
{
    return s.initialized && s.listen_session_active;
}

bool audio_service_is_busy(void)
{
    return s.initialized &&
           (s.listen_session_active ||
            s.play_state == PLAY_ACTIVE ||
            s.play_state == PLAY_BRIDGE_SAY ||
            s.bridge_say_playing ||
            audio_playback_service_v2_is_playing());
}

/* ── Bridge SAY (Etapa 12.2) ─────────────────────────────────────────────── */

void audio_service_bridge_say_chunk(const int16_t *samples, uint16_t count)
{
    if (!s.initialized || !samples || count == 0) return;
    if (count > NB_BRIDGE_AUDIO_CHUNK_SAMPLES) count = NB_BRIDGE_AUDIO_CHUNK_SAMPLES;

    xSemaphoreTake(s.mutex, portMAX_DELAY);
    bool listening = s.listen_session_active;
    if (listening) {
        s.play_state = PLAY_IDLE;
        s.bridge_say_playing = false;
        s.bridge_say_empty_ms = 0;
        (void)audio_playback_service_v2_say_cancel();
    }
    xSemaphoreGive(s.mutex);

    if (listening) {
        s_bridge_say_drop_count++;
        audio_playback_service_v2_note_say_dropped(0, true);
        if ((s_bridge_say_drop_count % 32U) == 1U) {
            ESP_LOGW(TAG, "Bridge SAY descartado durante escuta drops=%lu",
                     (unsigned long)s_bridge_say_drop_count);
        }
        return;
    }

    if (!s.bridge_say_playing) {
        xSemaphoreTake(s.mutex, portMAX_DELAY);
        s.play_state      = PLAY_BRIDGE_SAY;
        s.bridge_say_playing = true;
        s.bridge_say_empty_ms = 0;
        xSemaphoreGive(s.mutex);
        if (s.event_cb) s.event_cb(NB_AUDIO_EVT_PLAYBACK_START, 0);
        wake_service_rearm();
        ESP_LOGI(TAG, "Bridge SAY iniciado");
    }
    if (audio_playback_service_v2_say_enqueue(samples, count) == ESP_OK) {
        if ((++s_bridge_say_rx_count % 64U) == 1U) {
            ESP_LOGI(TAG, "Bridge SAY recebido chunks=%lu q=%u",
                     (unsigned long)s_bridge_say_rx_count,
                     (unsigned)audio_playback_service_v2_say_pending_count());
        }
        s.bridge_say_empty_ms = 0;
    } else {
        s_bridge_say_drop_count++;
        if ((s_bridge_say_drop_count % 32U) == 1U) {
            ESP_LOGW(TAG, "Bridge SAY dropado: fila cheia drops=%lu",
                     (unsigned long)s_bridge_say_drop_count);
        }
    }
}
