/*
 * vad_semantic_service.h — VAD semântico do NoiseBot (Layer 5)
 *
 * Analisa sessões de voz detectadas pelo audio_service e publica
 * eventos semânticos de alto nível:
 *
 *   NB_EVT_VOICE_SHORT          — interjeição < 500ms
 *   NB_EVT_VOICE_LONG           — discurso > 4s
 *   NB_EVT_VOICE_LOUD           — RMS médio > threshold (fala intensa)
 *   NB_EVT_VOICE_SOFT           — RMS médio < threshold (fala suave)
 *   NB_EVT_VOICE_FOLLOWUP_TIMEOUT — 8-12s sem nova voz após VOICE_END
 *   NB_EVT_VOICE_REPEATED       — 3 interjeições em < 30s
 *
 * Subscreve NB_EVT_VOICE_ACTIVITY_START/END internamente.
 * Tick chamado de behavior_task a cada 100ms.
 */

#ifndef NB_VAD_SEMANTIC_SERVICE_H
#define NB_VAD_SEMANTIC_SERVICE_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa o VAD semântico e subscreve eventos de voz.
 * @return ESP_OK ou ESP_ERR_INVALID_STATE se já inicializado.
 */
esp_err_t vad_semantic_init(void);

/**
 * @brief Avança timers e analisa energia de voz em curso.
 *
 * Chamar de behavior_task a cada dt_ms.
 */
void vad_semantic_tick(uint32_t dt_ms);

#ifdef __cplusplus
}
#endif

#endif /* NB_VAD_SEMANTIC_SERVICE_H */
