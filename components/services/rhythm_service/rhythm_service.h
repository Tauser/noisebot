/*
 * rhythm_service.h — Detecção de BPM por autocorrelação RMS (Layer 5)
 *
 * Amostra sound_analysis_get_rms() a cada tick (100ms) e calcula
 * autocorrelação sobre os últimos 6.4s. Range válido: 60–200 BPM.
 *
 * Eventos publicados (event bus):
 *   NB_EVT_RHYTHM_LOCKED  — confiança cruzou 0.65 (ritmo estável)
 *   NB_EVT_BEAT_TICK      — cada batida detectada enquanto LOCKED
 *   NB_EVT_RHYTHM_LOST    — confiança < 0.30 por > 2s
 *
 * Consumidores principais:
 *   led_service   — BEAT_TICK → led_effect_beat()
 *   boot_manager  — BEAT_TICK + IDLE → head-bob sutil via gaze
 *   idle_service  — rhythm_service_is_locked() → suprime yawn
 */

#ifndef NB_RHYTHM_SERVICE_H
#define NB_RHYTHM_SERVICE_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa o rhythm_service.
 *
 * @return ESP_OK ou ESP_ERR_INVALID_STATE se já inicializado.
 */
esp_err_t rhythm_service_init(void);

/**
 * @brief Avança detecção: amostra RMS, atualiza autocorrelação, dispara eventos.
 *
 * Chamar de behavior_task a cada dt_ms (tipicamente 100ms).
 */
void rhythm_service_tick(uint32_t dt_ms);

/** @brief Retorna BPM detectado (0 se não locked). */
float rhythm_service_get_bpm(void);

/** @brief Retorna confiança atual [0.0, 1.0]. */
float rhythm_service_get_confidence(void);

/** @brief Retorna true se o ritmo está estável (confiança > threshold). */
bool rhythm_service_is_locked(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_RHYTHM_SERVICE_H */
