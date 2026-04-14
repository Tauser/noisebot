/*
 * audio_hal.h — Driver I2S para microfone INMP441 e speaker MAX98357A
 *
 * I2S0 full-duplex: BCLK=GPIO41, WS=GPIO42 compartilhados.
 * RX: INMP441 em GPIO14, 32 bits/frame, 24 bits úteis no canal esquerdo.
 * TX: MAX98357A em GPIO1, aceita 32 bits, SD_MODE tied HIGH externamente.
 *
 * Formato no fio: Philips I2S, stereo, 32 bits/slot, 16kHz.
 *   - Mic:     int32_t raw[frame] = (audio_24bit << 8), canal L (par 0,2,4,…)
 *   - Speaker: int16_t mono → int32_t estéreo (shift left 16, canal L=R)
 *
 * Buffer DMA: NB_AUDIO_DMA_DESC_NUM × NB_AUDIO_CHUNK_FRAMES frames no SRAM.
 * Conversão estéreo↔mono feita internamente — caller usa mono em ambas as APIs.
 *
 * Uso: chamado exclusivamente pela audio_task. Não é thread-safe.
 *
 * Inicialização: audio_hal_init() deve ser chamado de audio_service_init()
 * em PHASE_SERVICES.
 */

#ifndef NB_AUDIO_HAL_H
#define NB_AUDIO_HAL_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Parâmetros de DMA ───────────────────────────────────────────────────── */

/** Frames por lote de DMA (256 @ 16kHz = 16ms). */
#define NB_AUDIO_CHUNK_FRAMES  256U

/** Número de descritores DMA (profundidade do ring buffer: 4×256=64ms). */
#define NB_AUDIO_DMA_DESC_NUM  4U

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa I2S0 em modo full-duplex (mic + speaker).
 *
 * Cria canais TX e RX no I2S_NUM_0, configura Philips 32-bit stereo 16kHz,
 * e habilita ambos os canais DMA.
 *
 * @return ESP_OK em sucesso. ESP_ERR_INVALID_STATE se já inicializado.
 */
esp_err_t audio_hal_init(void);

/**
 * @brief Lê amostras do microfone (canal esquerdo, canal direito descartado).
 *
 * Bloqueia até que `num_samples` frames estejam disponíveis no buffer DMA.
 * Extrai o canal esquerdo (INMP441) e converte de 32-bit (audio<<8) para
 * int32_t com valor centrado em zero.
 *
 * @param mono_buf    Buffer de saída. Deve ter pelo menos `num_samples` entradas.
 * @param num_samples Número de amostras a ler. Máximo NB_AUDIO_CHUNK_FRAMES.
 * @param samples_out Número de amostras efetivamente lidas.
 * @param ticks       Timeout em ticks do FreeRTOS (portMAX_DELAY = sem timeout).
 * @return            ESP_OK, ESP_ERR_TIMEOUT ou ESP_ERR_INVALID_STATE.
 */
esp_err_t audio_hal_mic_read(int32_t *mono_buf,
                              size_t   num_samples,
                              size_t  *samples_out,
                              TickType_t ticks);

/**
 * @brief Escreve amostras mono no speaker (convertido para estéreo internamente).
 *
 * Converte int16_t mono → int32_t estéreo (left-align, L=R) e escreve no
 * buffer DMA de TX. Bloqueia até que haja espaço no buffer.
 *
 * @param mono_buf    Buffer de entrada. Deve ter `num_samples` entradas.
 * @param num_samples Número de amostras a escrever. Máximo NB_AUDIO_CHUNK_FRAMES.
 * @param ticks       Timeout em ticks do FreeRTOS.
 * @return            ESP_OK, ESP_ERR_TIMEOUT ou ESP_ERR_INVALID_STATE.
 */
esp_err_t audio_hal_spk_write(const int16_t *mono_buf,
                               size_t         num_samples,
                               TickType_t     ticks);

/**
 * @brief Escreve silêncio (zeros) no buffer TX do speaker.
 *
 * Necessário em full-duplex para evitar underflow de TX quando não há áudio.
 *
 * @param num_frames Número de frames a escrever. Máximo NB_AUDIO_CHUNK_FRAMES.
 * @param ticks      Timeout em ticks.
 * @return           ESP_OK ou ESP_ERR_TIMEOUT.
 */
esp_err_t audio_hal_spk_write_silence(size_t num_frames, TickType_t ticks);

/**
 * @brief Libera os canais I2S e desinicializa o driver.
 *
 * Chamado apenas em shutdown controlado ou teste.
 */
void audio_hal_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_AUDIO_HAL_H */
