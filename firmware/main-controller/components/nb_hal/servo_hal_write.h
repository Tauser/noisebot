/*
 * servo_hal_write.h — Funções de escrita do driver SCS0009.
 *
 * Header privado: deve ser incluído APENAS por motion_service e motion_safety.
 * Todo acesso de escrita a servos fora desses dois componentes viola a regra
 * de arquitetura — toda posição escrita deve passar por motion_safety_check_position().
 *
 * Uso correto:
 *   motion_safety.c  — disable_torque no caminho de emergência
 *   motion_service.c — enable_torque + write_position após veto do safety
 */

#ifndef NB_SERVO_HAL_WRITE_H
#define NB_SERVO_HAL_WRITE_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * servo_hal_write_position() — Envia posição-alvo e tempo para o servo.
 *
 * Instrução WRITE nos registradores GOAL_POSITION_L (0x2A) e GOAL_TIME_L (0x2C).
 * Fire-and-forget: não aguarda resposta do servo.
 * DEVE ser precedido por motion_safety_check_position() no chamador.
 *
 * @param id       ID do servo.
 * @param pos      Posição alvo em unidades brutas (0–1023).
 * @param time_ms  Tempo para atingir a posição em ms (0 = máx velocidade).
 * @return         ESP_OK em sucesso, ESP_ERR_INVALID_STATE se não inicializado.
 */
esp_err_t servo_hal_write_position(uint8_t id, uint16_t pos, uint16_t time_ms);

/**
 * servo_hal_disable_torque() — Desabilita o torque do motor do servo.
 *
 * Escreve 0 no registrador TORQUE_ENABLE (0x28).
 * Após este comando o servo é livre para ser movido manualmente.
 * Fire-and-forget.
 *
 * @param id  ID do servo.
 * @return    ESP_OK em sucesso.
 */
esp_err_t servo_hal_disable_torque(uint8_t id);

/**
 * servo_hal_enable_torque() — Habilita o torque do motor do servo.
 *
 * Escreve 1 no registrador TORQUE_ENABLE (0x28).
 * DEVE ser chamado apenas após motion_safety_arm() ter sucedido.
 *
 * @param id  ID do servo.
 * @return    ESP_OK em sucesso.
 */
esp_err_t servo_hal_enable_torque(uint8_t id);

#ifdef __cplusplus
}
#endif

#endif /* NB_SERVO_HAL_WRITE_H */
