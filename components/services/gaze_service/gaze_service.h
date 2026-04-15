/*
 * gaze_service.h — Gaze system do NoiseBot (Layer 5)
 *
 * Modelo EMO — sem pupila. O gaze é expresso como deslocamento do shape dos
 * olhos (via expression_service_set_gaze): translação horizontal bilateral e
 * offset vertical. O tilt de pescoço é stub — ativado quando motion_service
 * (Etapa 3.3) for liberado.
 *
 * Dinâmica de saccade:
 *   Fase rápida  (60ms): current → target + 10% overshoot (linear).
 *   Fase de settle (150ms): overshoot → target (ease-out quadrático).
 *   Micro-drift contínuo: passeio gaussiano low-pass, amplitude ≤ 0.06.
 *
 * Atualização: render layer z=5 (Core 1, ~30fps).
 * API de target: thread-safe — pode ser chamada de qualquer task.
 */

#ifndef NB_GAZE_SERVICE_H
#define NB_GAZE_SERVICE_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa o gaze service e registra o render layer (z=5).
 *
 * Deve ser chamado após render_service_start() e expression_service_init().
 * @return ESP_OK ou ESP_ERR_INVALID_STATE se já inicializado.
 */
esp_err_t gaze_service_init(void);

/**
 * @brief Define o alvo do próximo saccade.
 *
 * Thread-safe. O saccade começa no próximo frame do render layer.
 *
 * @param x  [-1, 1]  -1=esquerda, +1=direita
 * @param y  [-1, 1]  -1=cima,    +1=baixo
 */
void gaze_service_set_target(float x, float y);

/**
 * @brief Retorna a posição atual do gaze.
 *
 * Leitura best-effort — sem lock (valores são float, atualizados atomicamente
 * no render_task Core 1; leitura de outro core pode pegar valor parcialmente
 * atualizado, mas a consequência é apenas um frame de gaze levemente impreciso).
 */
void gaze_service_get_current(float *x, float *y);

#ifdef __cplusplus
}
#endif

#endif /* NB_GAZE_SERVICE_H */
