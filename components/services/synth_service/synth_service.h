/*
 * synth_service.h — Síntese procedural de áudio (Layer 4)
 *
 * Gera áudio PCM 16-bit mono 16kHz a partir de osciladores (sine, square,
 * sawtooth, noise filtrado) com envelope ADSR e parâmetros levemente
 * randomizados a cada chamada (nenhum som idêntico).
 *
 * Integração com audio_service:
 *   O audio_service chama synth_fill_chunk() na audio_task antes de
 *   escrever silêncio. O synth_service nunca escreve no I2S diretamente —
 *   a audio_task é o único writer. Isso garante a exclusão mútua com WAV:
 *   enquanto um arquivo estiver em reprodução, synth_fill_chunk não é
 *   chamado.
 *
 * Mapamento de emoção:
 *   synth_play_for_emotion() usa nb_synth_emotion_hint_t (valores iguais
 *   a nb_expression_t) para evitar dependência Layer 4 → Layer 5.
 *   O caller faz cast: synth_play_for_emotion((nb_synth_emotion_hint_t)expr).
 *
 * Nota sobre melody:
 *   synth_melody() copia até SYNTH_MAX_MELODY_NOTES notas internamente —
 *   o array do caller pode ser stack-allocated.
 */

#ifndef NB_SYNTH_SERVICE_H
#define NB_SYNTH_SERVICE_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constantes ──────────────────────────────────────────────────────────── */

/** Máximo de notas suportadas em synth_melody(). */
#define SYNTH_MAX_MELODY_NOTES 8U

/* ── Tipos ───────────────────────────────────────────────────────────────── */

typedef enum {
    NB_SYNTH_TIMBRE_SINE   = 0, /**< Sinusoide pura                          */
    NB_SYNTH_TIMBRE_SQUARE = 1, /**< Onda quadrada                           */
    NB_SYNTH_TIMBRE_SAW    = 2, /**< Dente-de-serra                          */
    NB_SYNTH_TIMBRE_NOISE  = 3, /**< Ruído branco LP filtrado + LFO          */
} nb_synth_timbre_t;

/** Uma nota para synth_melody(). */
typedef struct {
    float    freq;        /**< Frequência em Hz (0 = pausa/silêncio)         */
    uint32_t duration_ms; /**< Duração da nota em ms                         */
} nb_note_t;

/**
 * Hint de emoção para synth_play_for_emotion().
 * Valores intencionalmente idênticos a nb_expression_t para cast seguro.
 */
typedef enum {
    NB_SYNTH_NEUTRAL    = 0,
    NB_SYNTH_HAPPY      = 1,
    NB_SYNTH_CURIOUS    = 2,
    NB_SYNTH_SLEEPY     = 3,
    NB_SYNTH_FOCUSED    = 4,
    NB_SYNTH_SUSPICIOUS = 5,
    NB_SYNTH_SURPRISED  = 6,
    NB_SYNTH_SAD        = 7,
    NB_SYNTH_ALARMED    = 8,
} nb_synth_emotion_hint_t;

/* ── API de inicialização ────────────────────────────────────────────────── */

/**
 * @brief Inicializa o synth_service.
 *
 * Cria mutex interno. Sem task, sem alocação dinâmica de buffers.
 *
 * @return ESP_OK ou ESP_ERR_NO_MEM (mutex falhou).
 */
esp_err_t synth_init(void);

/* ── API interna (chamada pelo audio_service) ────────────────────────────── */

/**
 * @brief Gera o próximo chunk de áudio sintético.
 *
 * Chamado pela audio_task a cada 16ms, APENAS quando não há WAV em
 * reprodução. Preenche out_buf com `samples` amostras PCM 16-bit.
 *
 * @param out_buf Buffer de saída (deve ter `samples` int16_t alocados).
 * @param samples Número de amostras desejadas (normalmente 256).
 * @return true se o synth estava ativo e preencheu out_buf.
 *         false se idle (caller deve escrever silêncio).
 */
bool synth_fill_chunk(int16_t *out_buf, size_t samples);

/* ── Primitivos expressivos ──────────────────────────────────────────────── */

/**
 * @brief Tom com varredura de frequência (chirp / sweep).
 *
 * Interpolação linear de freq_start → freq_end ao longo de duration_ms.
 * Parâmetros levemente randomizados a cada chamada (±8%).
 *
 * @param freq_start Frequência inicial em Hz.
 * @param freq_end   Frequência final em Hz.
 * @param duration_ms Duração em ms.
 */
void synth_chirp(float freq_start, float freq_end, uint32_t duration_ms);

/**
 * @brief Ronronar contínuo (noise LP filtrado com LFO de amplitude).
 *
 * @param duration_ms Duração em ms.
 * @param intensity   Amplitude 0.0–1.0.
 */
void synth_purr(uint32_t duration_ms, float intensity);

/**
 * @brief Tom curto com decay rápido.
 *
 * @param freq        Frequência em Hz.
 * @param duration_ms Duração total em ms (inclui decay).
 */
void synth_blip(float freq, uint32_t duration_ms);

/**
 * @brief Toca uma sequência de notas.
 *
 * Copia até SYNTH_MAX_MELODY_NOTES notas internamente (array pode ser
 * stack-allocated pelo caller). Notas com freq=0 são pausas.
 *
 * @param notes Array de notas.
 * @param count Número de notas (clamped a SYNTH_MAX_MELODY_NOTES).
 */
void synth_melody(const nb_note_t *notes, uint8_t count);

/**
 * @brief Define o timbre padrão para os próximos primitivos.
 *
 * Persiste até a próxima chamada. Não afeta o som em reprodução.
 */
void synth_set_timbre(nb_synth_timbre_t t);

/**
 * @brief Toca o som característico de uma emoção, com parâmetros
 *        randomizados para que cada chamada soe ligeiramente diferente.
 *
 * Cast seguro: synth_play_for_emotion((nb_synth_emotion_hint_t)nb_expression);
 */
void synth_play_for_emotion(nb_synth_emotion_hint_t hint);

/**
 * @brief Define o volume do synth (0–100, mesmo range que audio_set_volume).
 *
 * Default: 80. Thread-safe.
 */
void synth_set_volume(uint8_t level);

/**
 * @brief Para o som atual imediatamente (sem fade-out).
 */
void synth_stop(void);

/**
 * @brief Retorna true se o synth está gerando amostras.
 */
bool synth_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_SYNTH_SERVICE_H */
