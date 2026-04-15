/*
 * audio_service.h — Serviço de áudio do NoiseBot (Layer 4)
 *
 * Responsabilidades:
 *   - VAD (Voice Activity Detection): lê mic continuamente, notifica via callback
 *     sobre NB_AUDIO_EVT_VOICE_START e NB_AUDIO_EVT_VOICE_END.
 *   - Playback de WAV: streaming em chunks de 256 amostras do SD, sem OOM.
 *   - Controle de volume: 0–100, aplicado digitalmente no PCM.
 *   - Gravação de diagnóstico: N segundos de PCM salvo como WAV no SD.
 *
 * VAD:
 *   RMS calculado sobre janelas de NB_AUDIO_CHUNK_FRAMES (256 @ 16kHz = 16ms).
 *   Threshold = NB_AUDIO_VAD_THRESHOLD_DEFAULT (ajustável via setter).
 *   VOICE_START: quando RMS > threshold.
 *   VOICE_END: após NB_AUDIO_VAD_SILENCE_MS contínuos de silêncio.
 *
 * Playback:
 *   Suporta WAV PCM 16-bit mono 16kHz. Outros formatos retornam erro.
 *   audio_play_file() é não-bloqueante — retorna imediatamente.
 *   Callbacks NB_AUDIO_EVT_PLAYBACK_START (data=duration_ms) e
 *   NB_AUDIO_EVT_PLAYBACK_END emitidos para publicar no event bus.
 *
 * Eventos são reportados via callback (nb_audio_event_cb_t) para desacoplar
 * Layer 4 do event bus em infra. O boot_manager registra a ponte.
 *
 * Task: "nb_audio_task"  Core: 0  Prioridade: 6  Stack: 4096
 *
 * Dependência de inicialização:
 *   Chamar audio_service_init() em PHASE_SERVICES, após SD estar disponível.
 */

#ifndef NB_AUDIO_SERVICE_H
#define NB_AUDIO_SERVICE_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Parâmetros de VAD ───────────────────────────────────────────────────── */

/**
 * Threshold de energia RMS para início de atividade de voz.
 * Unidade: valor int32_t signed, 24-bit centrado em zero (pós-shift do INMP441).
 * Default calibrado para ambiente de escritório silencioso.
 */
#define NB_AUDIO_VAD_THRESHOLD_DEFAULT   80000

/** Silêncio contínuo em ms antes de emitir VOICE_END. */
#define NB_AUDIO_VAD_SILENCE_MS          300U

/**
 * Período de settling pós-init: VAD não emite eventos durante este tempo.
 * O mic pega ruído de boot (PSU, circuitos inicializando) e os primeiros
 * VAD_STARTs são quase sempre falsos positivos.
 */
#define NB_AUDIO_VAD_SETTLE_MS          5000U

/* ── Eventos de áudio ────────────────────────────────────────────────────── */

typedef enum {
    NB_AUDIO_EVT_VOICE_START,      /**< Atividade de voz detectada          */
    NB_AUDIO_EVT_VOICE_END,        /**< Silêncio após atividade de voz      */
    NB_AUDIO_EVT_PLAYBACK_START,   /**< Reprodução iniciada; data=duration_ms */
    NB_AUDIO_EVT_PLAYBACK_END,     /**< Reprodução terminada (EOF ou stop)  */
} nb_audio_event_t;

/**
 * Callback chamado pela audio_task ao emitir eventos.
 * @param evt  Tipo do evento.
 * @param data Dado associado (0 exceto para PLAYBACK_START: duration_ms).
 */
typedef void (*nb_audio_event_cb_t)(nb_audio_event_t evt, uint32_t data);

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o serviço de áudio e cria a audio_task.
 *
 * Chama audio_hal_init() internamente.
 * Volume inicial = 80 (pode ser ajustado via audio_set_volume após o init).
 *
 * @return ESP_OK, ESP_FAIL (HAL init) ou ESP_ERR_NO_MEM (task create).
 */
esp_err_t audio_service_init(void);

/**
 * @brief Registra o callback de eventos de áudio.
 *
 * Deve ser chamado após audio_service_init(). Substitui qualquer callback
 * anterior. Pode ser NULL para desregistrar.
 *
 * @param cb Função callback. Chamada da audio_task (Core 0).
 */
void audio_service_set_event_cb(nb_audio_event_cb_t cb);

/* ── Playback ─────────────────────────────────────────────────────────────── */

/**
 * @brief Inicia reprodução de arquivo WAV do SD.
 *
 * Não-bloqueante. Sinaliza a audio_task para iniciar o streaming.
 * Se outro arquivo estiver tocando, para o atual e inicia o novo.
 *
 * Requisitos: WAV PCM 16-bit mono 16kHz.
 *
 * @param path Caminho absoluto no SD (ex: "/sdcard/assets/audio/greet_01.wav").
 * @return     ESP_OK se comando aceito. ESP_ERR_INVALID_STATE se não iniciado.
 */
esp_err_t audio_play_file(const char *path);

/**
 * @brief Inicia reprodução de arquivo PCM raw do SD (sem cabeçalho WAV).
 *
 * Assume formato fixo: 16kHz, mono, 16-bit signed little-endian.
 * Não-bloqueante. Comportamento idêntico a audio_play_file().
 *
 * @param path Caminho absoluto no SD (ex: "/sdcard/assets/audio/greet.pcm").
 * @return     ESP_OK se comando aceito. ESP_ERR_INVALID_STATE se não iniciado.
 */
esp_err_t audio_play_pcm_raw(const char *path);

/**
 * @brief Para a reprodução em andamento.
 *
 * Não-bloqueante. Emite NB_AUDIO_EVT_PLAYBACK_END via callback.
 */
esp_err_t audio_play_stop(void);

/**
 * @brief Retorna true se há reprodução em andamento.
 */
bool audio_is_playing(void);

/* ── Volume ───────────────────────────────────────────────────────────────── */

/**
 * @brief Define o nível de volume (0–100).
 *
 * Aplicado digitalmente ao PCM. Não persiste em NVS — caller responsável.
 * 0 = silêncio, 100 = sem atenuação.
 *
 * @return ESP_ERR_INVALID_ARG se level > 100.
 */
esp_err_t audio_set_volume(uint8_t level);

/**
 * @brief Retorna o nível de volume atual (0–100).
 */
uint8_t audio_get_volume(void);

/* ── VAD ──────────────────────────────────────────────────────────────────── */

/**
 * @brief Ajusta o threshold de detecção de voz em runtime.
 *
 * @param threshold Valor de RMS (int32_t 24-bit). Ver NB_AUDIO_VAD_THRESHOLD_DEFAULT.
 */
void audio_service_set_vad_threshold(int32_t threshold);

/* ── Diagnóstico ─────────────────────────────────────────────────────────── */

/**
 * @brief Inicia gravação de diagnóstico do microfone para o SD.
 *
 * Não-bloqueante. Grava como WAV 16-bit mono 16kHz.
 * Não interrompe VAD nem playback em andamento.
 *
 * @param path       Caminho de destino (ex: "/sdcard/logs/mic_diag.wav").
 * @param duration_s Duração em segundos (1–10).
 * @return           ESP_OK, ESP_ERR_INVALID_STATE ou ESP_ERR_INVALID_ARG.
 */
esp_err_t audio_record_diagnostic(const char *path, uint32_t duration_s);

#ifdef __cplusplus
}
#endif

#endif /* NB_AUDIO_SERVICE_H */
