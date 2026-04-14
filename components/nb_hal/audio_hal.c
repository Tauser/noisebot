/*
 * audio_hal.c — Driver I2S para INMP441 (mic) e MAX98357A (speaker)
 *
 * I2S0 full-duplex: Philips format, 32-bit stereo, 16kHz.
 *
 * Buffers internos estáticos em SRAM:
 *   s_rx_stereo: NB_AUDIO_CHUNK_FRAMES × 2 × int32_t = 2048 bytes
 *   s_tx_stereo: NB_AUDIO_CHUNK_FRAMES × 2 × int32_t = 2048 bytes
 *
 * Thread safety: não é thread-safe — chamado exclusivamente pela audio_task.
 */

#include "audio_hal.h"
#include "nb_hw_config.h"
#include "esp_log.h"

#include "driver/i2s_std.h"
#include "driver/i2s_common.h"

#include <string.h>

#define TAG "audio_hal"

/* ── Handles I2S ─────────────────────────────────────────────────────────── */

static i2s_chan_handle_t s_tx = NULL;
static i2s_chan_handle_t s_rx = NULL;
static bool              s_initialized = false;

/* ── Buffers estáticos de conversão (SRAM) ───────────────────────────────── */

/* Tamanho em bytes: frames × 2 canais × 4 bytes/sample */
#define STEREO_BUF_BYTES (NB_AUDIO_CHUNK_FRAMES * 2U * sizeof(int32_t))

static int32_t s_rx_stereo[NB_AUDIO_CHUNK_FRAMES * 2];
static int32_t s_tx_stereo[NB_AUDIO_CHUNK_FRAMES * 2];
static int32_t s_silence  [NB_AUDIO_CHUNK_FRAMES * 2];  /* zeros — alimenta TX em idle */

/* ── Init ────────────────────────────────────────────────────────────────── */

esp_err_t audio_hal_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "audio_hal_init chamado novamente — ignorado");
        return ESP_ERR_INVALID_STATE;
    }

    /* s_silence é inicializado com zeros pelo linker (BSS) — nada a fazer. */

    /* ── 1. Criar canais TX e RX no mesmo port ── */
    i2s_chan_config_t chan_cfg = {
        .id            = NB_AUDIO_I2S_PORT,
        .role          = I2S_ROLE_MASTER,
        .dma_desc_num  = NB_AUDIO_DMA_DESC_NUM,
        .dma_frame_num = NB_AUDIO_CHUNK_FRAMES,
        .auto_clear    = true,   /* TX: preenche com zeros em underflow */
    };

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel falhou: %s", esp_err_to_name(err));
        return err;
    }

    /* ── 2. Configurar modo Philips I2S 32-bit stereo 16kHz ── */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(NB_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)NB_AUDIO_PIN_BCLK,
            .ws   = (gpio_num_t)NB_AUDIO_PIN_WS,
            .dout = (gpio_num_t)NB_SPK_PIN_DIN,
            .din  = (gpio_num_t)NB_MIC_PIN_SD,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    /* TX channel */
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode TX falhou: %s", esp_err_to_name(err));
        goto fail_del;
    }

    /* RX channel — mesma config (BCLK/WS compartilhados, apenas DIN é diferente) */
    err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode RX falhou: %s", esp_err_to_name(err));
        goto fail_del;
    }

    /* ── 3. Habilitar ambos os canais ── */
    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable TX falhou: %s", esp_err_to_name(err));
        goto fail_del;
    }

    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable RX falhou: %s", esp_err_to_name(err));
        /* TX já está habilitado — desabilitar antes de deletar */
        i2s_channel_disable(s_tx);
        goto fail_del;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "I2S0 full-duplex inicializado: %uHz, 32-bit stereo, "
            "BCLK=%d WS=%d DIN=%d DOUT=%d",
            (unsigned)NB_AUDIO_SAMPLE_RATE,
            NB_AUDIO_PIN_BCLK, NB_AUDIO_PIN_WS,
            NB_MIC_PIN_SD, NB_SPK_PIN_DIN);
    return ESP_OK;

fail_del:
    i2s_del_channel(s_tx);
    i2s_del_channel(s_rx);
    s_tx = s_rx = NULL;
    return err;
}

/* ── Leitura do microfone ─────────────────────────────────────────────────── */

esp_err_t audio_hal_mic_read(int32_t   *mono_buf,
                              size_t     num_samples,
                              size_t    *samples_out,
                              TickType_t ticks)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (num_samples > NB_AUDIO_CHUNK_FRAMES || mono_buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t bytes_to_read = num_samples * 2U * sizeof(int32_t); /* stereo */
    size_t bytes_read    = 0;

    esp_err_t err = i2s_channel_read(s_rx, s_rx_stereo, bytes_to_read,
                                      &bytes_read, ticks);
    if (err != ESP_OK) return err;

    /*
     * INMP441 canal esquerdo: bytes_read / (2 × 4) frames.
     * Formato do frame: [L, R, L, R, ...] — L está em índice par.
     * Valor INMP441: audio_24bit << 8 no int32_t. Shift right 8 para
     * obter valor centrado em zero com magnitude de 24 bits.
     */
    size_t frames = bytes_read / (2U * sizeof(int32_t));
    for (size_t i = 0; i < frames; i++) {
        mono_buf[i] = s_rx_stereo[i * 2U] >> 8;   /* canal L, signed 24-bit */
    }

    if (samples_out) *samples_out = frames;
    return ESP_OK;
}

/* ── Escrita no speaker ───────────────────────────────────────────────────── */

esp_err_t audio_hal_spk_write(const int16_t *mono_buf,
                               size_t         num_samples,
                               TickType_t     ticks)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (num_samples > NB_AUDIO_CHUNK_FRAMES || mono_buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Converte int16_t mono → int32_t stereo.
     * Left-align: (int32_t)sample << 16.
     * Duplica em ambos os canais (MAX98357A mono, não importa qual lado).
     */
    for (size_t i = 0; i < num_samples; i++) {
        int32_t s32 = (int32_t)mono_buf[i] << 16;
        s_tx_stereo[i * 2U]      = s32;   /* canal L */
        s_tx_stereo[i * 2U + 1U] = s32;   /* canal R */
    }

    size_t bytes_to_write = num_samples * 2U * sizeof(int32_t);
    size_t bytes_written  = 0;

    return i2s_channel_write(s_tx, s_tx_stereo, bytes_to_write,
                              &bytes_written, ticks);
}

/* ── Silêncio no speaker ─────────────────────────────────────────────────── */

esp_err_t audio_hal_spk_write_silence(size_t num_frames, TickType_t ticks)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (num_frames > NB_AUDIO_CHUNK_FRAMES) return ESP_ERR_INVALID_ARG;

    size_t bytes = num_frames * 2U * sizeof(int32_t);
    size_t written = 0;
    return i2s_channel_write(s_tx, s_silence, bytes, &written, ticks);
}

/* ── Deinit ──────────────────────────────────────────────────────────────── */

void audio_hal_deinit(void)
{
    if (!s_initialized) return;

    i2s_channel_disable(s_tx);
    i2s_channel_disable(s_rx);
    i2s_del_channel(s_tx);
    i2s_del_channel(s_rx);
    s_tx = s_rx = NULL;
    s_initialized = false;
    ESP_LOGI(TAG, "I2S0 desinicializado");
}
