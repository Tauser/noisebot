/*
 * idle_service.h — Microbehaviors de idle do NoiseBot (Layer 5)
 *
 * Gera comportamentos probabilísticos quando o sistema está em IDLE ou ATTENTIVE.
 * Não possui task própria — atualizado via behavior_task (100ms).
 *
 * Behaviors:
 *   IDLE + ATTENTIVE:
 *     - Micro-saccade: a cada 5–15s, reposiciona o gaze via gaze_service.
 *   ATTENTIVE:
 *     - Aversive gaze: a cada 8–15s, olhar se desvia lateralmente.
 *   IDLE:
 *     - Yawn: expressão SLEEPY por ~2.5s a cada 60–180s.
 *
 *   Blink e LED breathing são gerenciados pelos próprios serviços (expression
 *   e led_service). idle_service não duplica esse controle.
 *
 *   Nota Etapa 3.3: micro-neck-movement (amplitude <5°) está preparado mas
 *   sem implementação até motion_service estar liberado.
 */

#ifndef NB_IDLE_SERVICE_H
#define NB_IDLE_SERVICE_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa o idle service com timers aleatórios.
 *
 * Deve ser chamado após gaze_service_init() e expression_service_init().
 * @return ESP_OK ou ESP_ERR_INVALID_STATE se já inicializado.
 */
esp_err_t idle_service_init(void);

/**
 * @brief Tick periódico — avança timers e dispara behaviors.
 *
 * Chamado pela behavior_task a cada dt_ms ms.
 *
 * @param dt_ms  Intervalo desde a última chamada, em ms.
 */
void idle_service_update(uint32_t dt_ms);

/* ── Timer de solidão ────────────────────────────────────────────────────── */

/**
 * Chamado quando o robot fica sozinho (sem interação) por ALONE_THRESHOLD_MS.
 * Registrado pelo boot_manager via idle_service_set_alone_cb().
 */
typedef void (*nb_idle_alone_cb_t)(void);

/**
 * @brief Registra callback de solidão.
 *
 * O callback é invocado uma vez ao atingir o threshold.
 * O timer é resetado automaticamente após o disparo.
 */
void idle_service_set_alone_cb(nb_idle_alone_cb_t cb);

/**
 * @brief Ajusta a velocidade dos micro-saccades.
 *
 * Chamar após persona_service_init() / persona_service_refresh() com
 * `curiosity > 0.6 ? 1.5f : 1.0f`. Factor > 1 = saccades mais frequentes.
 *
 * @param factor  Multiplicador de frequência (1.0 = padrão, 1.5 = +50%).
 */
void idle_service_set_saccade_multiplier(float factor);

/**
 * @brief Notifica o idle_service que houve interação (touch, voz, etc.).
 *
 * Reseta o timer de solidão. Deve ser chamado do boot_manager nos handlers
 * de TAP e VOICE_START.
 */
void idle_service_on_interaction(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_IDLE_SERVICE_H */
