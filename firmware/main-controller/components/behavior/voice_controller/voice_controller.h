/*
 * voice_controller.h - Politica de turn-taking de voz do NoiseBot (Layer 6)
 */

#ifndef NB_VOICE_CONTROLLER_H
#define NB_VOICE_CONTROLLER_H

#include "esp_err.h"
#include "state_machine.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa o controlador de voz.
 *
 * O controlador nao cria task. Ele centraliza side effects de wake/listening e
 * speaking para reduzir politica conversacional espalhada no boot_manager.
 */
esp_err_t voice_controller_init(void);

/**
 * @brief Processa wake word detectada.
 *
 * Retorna true quando o evento foi aceito ou consumido. Retorna false quando
 * deve ser ignorado pelo estado atual.
 */
bool voice_controller_on_wake_word_detected(void);

/**
 * @brief Solicita uma janela curta de follow-up apos playback do bridge.
 */
void voice_controller_request_followup_listen(void);

/**
 * @brief Aplica efeitos de entrada/saida de estado relacionados a voz.
 */
void voice_controller_on_state_changed(nb_robot_state_t new_state,
                                       nb_robot_state_t old_state);

#ifdef __cplusplus
}
#endif

#endif /* NB_VOICE_CONTROLLER_H */
