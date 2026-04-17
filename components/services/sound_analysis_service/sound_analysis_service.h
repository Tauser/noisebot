/*
 * sound_analysis_service.h — Análise de áudio em tempo real (Layer 4)
 *
 * Executa FFT de 256 pontos sobre janelas PCM do microfone (16ms @ 16kHz)
 * e classifica o conteúdo sonoro em 6 classes.
 *
 * Classes: SILENCE, VOICE, MUSIC, NOISE, CLAP, WHISTLE.
 *
 * Eventos publicados no bus (nb_events.h):
 *   NB_EVT_SOUND_CLAP          — palmas detectadas (evento imediato)
 *   NB_EVT_SOUND_WHISTLE       — assobio detectado (após 300ms)
 *   NB_EVT_SOUND_MUSIC_START   — música ambiente detectada (após 3s)
 *   NB_EVT_SOUND_MUSIC_END     — música parou (após 2s de silêncio)
 *   NB_EVT_SOUND_CLASS_CHANGED — transição de classe; data.u32 = nova classe
 *
 * Integração:
 *   sound_analysis_tick() é chamado pela audio_task (Core 0) a cada 16ms,
 *   logo após a leitura do microfone. Não tem task própria.
 *
 *   sound_analysis_get_*() são thread-safe: leem campos volatile atualizados
 *   atomicamente no tick.
 *
 * Dependências de init:
 *   Chamar sound_analysis_init() em PHASE_SERVICES, após event_bus_init().
 */

#ifndef NB_SOUND_ANALYSIS_SERVICE_H
#define NB_SOUND_ANALYSIS_SERVICE_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Tipos ───────────────────────────────────────────────────────────────── */

/**
 * Classe de som detectada.
 * Usada em NB_EVT_SOUND_CLASS_CHANGED (data.u32) e em sound_analysis_get_class().
 */
typedef enum {
    NB_SOUND_SILENCE  = 0, /**< Nível abaixo do threshold de atividade         */
    NB_SOUND_VOICE    = 1, /**< Energia concentrada na faixa de voz (125-3375Hz) */
    NB_SOUND_MUSIC    = 2, /**< Energia broadband sustentada (>3s)              */
    NB_SOUND_NOISE    = 3, /**< Ruído genérico acima do threshold               */
    NB_SOUND_CLAP     = 4, /**< Transiente broadband (palmas) — evento pontual  */
    NB_SOUND_WHISTLE  = 5, /**< Narrowband 1-3kHz sustentado (>300ms)           */
} nb_sound_class_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o serviço.
 *
 * Pré-computa a janela de Hann. Não cria task nem aloca memória dinâmica.
 *
 * @return ESP_OK ou ESP_ERR_INVALID_STATE se já inicializado.
 */
esp_err_t sound_analysis_init(void);

/**
 * @brief Processa um chunk de PCM mono 16-bit 16kHz.
 *
 * Deve ser chamado a cada 16ms pela audio_task, logo após a leitura do mic.
 * Espera exatamente NB_AUDIO_CHUNK_FRAMES (256) amostras.
 *
 * @param pcm     Amostras PCM 16-bit signed, mono, 16kHz.
 * @param samples Número de amostras (deve ser 256; menos é aceito mas degrada).
 */
void sound_analysis_tick(const int16_t *pcm, size_t samples);

/**
 * @brief Retorna a classe de som atual.
 *
 * Thread-safe (leitura de campo volatile).
 */
nb_sound_class_t sound_analysis_get_class(void);

/**
 * @brief Retorna o RMS normalizado do último chunk (0.0–1.0 aprox.).
 *
 * Thread-safe. 0.0 indica silêncio.
 */
float sound_analysis_get_rms(void);

/**
 * @brief Retorna a frequência dominante do último chunk em Hz.
 *
 * Thread-safe. 0.0 se silêncio ou não inicializado.
 */
float sound_analysis_get_dominant_freq(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_SOUND_ANALYSIS_SERVICE_H */
