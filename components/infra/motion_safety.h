/*
 * motion_safety.h — Safety layer para servos SCS0009
 *
 * Autoridade de veto sobre qualquer comando de posição ou velocidade.
 * Toda escrita de posição DEVE passar por motion_safety_check_position().
 *
 * Estados:
 *   DISABLED      — padrão após init. Nenhum movimento permitido.
 *   INITIALIZING  — arm() em progresso (PING + leitura de telemetria).
 *   ARMED         — operacional. Movimentos permitidos com validação.
 *   FAULT         — falha permanente. Só reset do sistema pode sair daqui.
 *
 * Monitoramento contínuo (nb_servo_safety_task, 20Hz):
 *   Load  : WARN >40%, DISABLE >70% por >100ms (stall detection)
 *   Temp  : WARN ≥55°C, DISABLE ≥70°C
 *   Heartbeat: timeout de 600ms sem chamada a motion_safety_heartbeat()
 *
 * Brownout: subscreve NB_EVT_POWER_BROWNOUT_WARN — disable imediato.
 *
 * Task: "nb_safety_task"  Core: 1  Prioridade: 23  Stack: 4096
 *
 * Dependências: servo_hal (nb_hal), event_bus, config_manager, logger (infra).
 * Localização em infra/ para evitar dependência circular:
 *   infra → safety não pode existir se safety → infra.
 */

#ifndef NB_MOTION_SAFETY_H
#define NB_MOTION_SAFETY_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Tipos ───────────────────────────────────────────────────────────────── */

typedef enum {
    NB_MOTION_DISABLED = 0,   /**< Padrão. Nenhum movimento permitido.        */
    NB_MOTION_INITIALIZING,   /**< arm() em progresso.                        */
    NB_MOTION_ARMED,          /**< Operacional. Movimentos com validação.     */
    NB_MOTION_FAULT,          /**< Falha permanente. Requer reset do sistema. */
} nb_motion_state_t;

/* Velocidade máxima permitida em steps/segundo (SCS0009, 300°/1023 steps) */
#define NB_MOTION_MAX_VELOCITY_STEPS_PER_S   300u

/* Limiares de load (raw 0–1023 ~ 0–100%) */
#define NB_MOTION_LOAD_WARN_THRESHOLD        409u  /* ~40% de 1023 */
#define NB_MOTION_LOAD_FAULT_THRESHOLD       716u  /* ~70% de 1023 */
#define NB_MOTION_LOAD_FAULT_DURATION_MS     100u  /* >70% por 100ms → stall */

/* Limiares de temperatura (°C) */
#define NB_MOTION_TEMP_WARN_C                 55u
#define NB_MOTION_TEMP_FAULT_C                70u

/* Heartbeat */
#define NB_MOTION_HEARTBEAT_FEED_MS          200u  /* intervalo esperado */
#define NB_MOTION_HEARTBEAT_TIMEOUT_MS       600u  /* timeout → fault    */

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * motion_safety_init() — Inicializa o módulo de safety.
 *
 * Cria mutex interno, subscreve NB_EVT_POWER_BROWNOUT_WARN no event bus
 * e cria a nb_safety_task em Core 1, prioridade 23.
 *
 * Deve ser chamado em PHASE_SAFETY, após servo_hal_init() e nb_event_bus_init().
 * Estado resultante: NB_MOTION_DISABLED.
 *
 * @return ESP_OK em sucesso.
 */
esp_err_t motion_safety_init(void);

/**
 * motion_safety_arm() — Tenta armar o sistema de motion.
 *
 * Sequência:
 *   1. Valida estado atual = DISABLED (erro se FAULT ou já ARMED).
 *   2. Transição → INITIALIZING.
 *   3. PING em ambos os servos via servo_hal_ping().
 *   4. READ posição de ambos (sanity check).
 *   5. Se OK → ARMED + publish NB_EVT_MOTION_ARMED.
 *   6. Se falha → DISABLED (não vai para FAULT — permite re-tentativa).
 *
 * Reinicia o heartbeat timer ao armar com sucesso.
 *
 * @return ESP_OK se ARMED, ESP_FAIL se servos não responderam,
 *         ESP_ERR_INVALID_STATE se em FAULT ou já ARMED.
 */
esp_err_t motion_safety_arm(void);

/**
 * motion_safety_check_position() — Valida posição antes de enviar ao servo.
 *
 * Autoridade de veto: se retornar != ESP_OK, o chamador NÃO deve enviar o
 * comando de posição ao servo_hal.
 *
 * @param id   ID do servo (1=PAN, 2=TILT).
 * @param pos  Posição candidata (0–1023).
 * @return     ESP_OK se dentro dos limites NVS.
 *             ESP_ERR_INVALID_ARG se pos fora de [srv_min, srv_max].
 *             ESP_ERR_INVALID_STATE se não ARMED.
 */
esp_err_t motion_safety_check_position(uint8_t id, uint16_t pos);

/**
 * motion_safety_check_velocity() — Valida velocidade (steps/segundo).
 *
 * @param id              ID do servo.
 * @param vel_steps_per_s Velocidade candidata.
 * @return                ESP_OK se ≤ NB_MOTION_MAX_VELOCITY_STEPS_PER_S.
 *                        ESP_ERR_INVALID_ARG se exceder o limite.
 *                        ESP_ERR_INVALID_STATE se não ARMED.
 */
esp_err_t motion_safety_check_velocity(uint8_t id, uint16_t vel_steps_per_s);

/**
 * motion_safety_heartbeat() — Alimenta o watchdog de heartbeat.
 *
 * Deve ser chamado pela motion_task a cada ~NB_MOTION_HEARTBEAT_FEED_MS ms
 * enquanto o sistema estiver ARMED. Ausência por >600ms → FAULT automático.
 *
 * Thread-safe. Pode ser chamado de qualquer task.
 */
void motion_safety_heartbeat(void);

/**
 * motion_safety_get_state() — Retorna o estado atual.
 *
 * Leitura atômica — não bloqueia.
 */
nb_motion_state_t motion_safety_get_state(void);

/**
 * motion_safety_is_armed() — Retorna true se estado == NB_MOTION_ARMED.
 */
bool motion_safety_is_armed(void);

/**
 * motion_safety_emergency_stop() — Para servos imediatamente antes de reiniciar.
 *
 * Projetado para uso em NB_ASSERT_SAFETY, chamado logo antes de esp_restart().
 * Idempotente e thread-safe. Seguro chamar antes de motion_safety_init()
 * (retorna imediatamente sem efeito se não inicializado).
 *
 * Ação: park_and_disable() + transição para FAULT.
 * O delay de 100ms em NB_ASSERT_SAFETY permite que o disable de torque
 * seja processado pelo servo antes do reset.
 */
void motion_safety_emergency_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_MOTION_SAFETY_H */
