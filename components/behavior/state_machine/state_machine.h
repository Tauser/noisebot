/*
 * state_machine.h — State machine do NoiseBot (Layer 6)
 *
 * Gerencia o estado de alto nível do robot.
 * Não depende de infra diretamente — recebe eventos via chamadas diretas
 * do boot_manager, que faz a ponte entre o event bus e os handlers abaixo.
 *
 * Estados:
 *   BOOT_UP        — Inicializando. Transita para IDLE em boot_complete.
 *   IDLE           — Em repouso. Timeout → SLEEPING.
 *   ATTENTIVE      — Ouvindo voz.
 *   RESPONDING     — Reproduzindo áudio.
 *   TOUCH_REACTING — Respondendo a toque (transitório, ~2s).
 *   SLEEPING       — Inativo. Toque ou voz → IDLE.
 *   ERROR          — Falha de safety. Requer reset para sair.
 *   SAFE_MODE      — Safe mode de boot. Sem motion.
 *
 * Transições loggadas com timestamp e motivo em cada mudança.
 * Thread-safe via spinlock (seções críticas curtas).
 */

#ifndef NB_STATE_MACHINE_H
#define NB_STATE_MACHINE_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Estados ─────────────────────────────────────────────────────────────── */

typedef enum {
    NB_STATE_BOOT_UP        = 0,
    NB_STATE_IDLE           = 1,
    NB_STATE_ATTENTIVE      = 2,
    NB_STATE_RESPONDING     = 3,
    NB_STATE_TOUCH_REACTING = 4,
    NB_STATE_SLEEPING       = 5,
    NB_STATE_ERROR          = 6,
    NB_STATE_SAFE_MODE      = 7,
    NB_STATE_COUNT          = 8,
} nb_robot_state_t;

/* ── Callback de transição ───────────────────────────────────────────────── */

/**
 * Chamado em cada transição de estado.
 * Executado in-task de quem chamou o input handler.
 * Deve ser rápido — não bloquear.
 *
 * @param new_state  Estado após a transição.
 * @param old_state  Estado anterior.
 * @param reason     String estática descrevendo a causa.
 */
typedef void (*nb_state_change_cb_t)(nb_robot_state_t new_state,
                                      nb_robot_state_t old_state,
                                      const char      *reason);

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa a state machine.
 *
 * Estado inicial: NB_STATE_BOOT_UP (ou SAFE_MODE se start_safe_mode=true).
 * Deve ser chamado em PHASE_SERVICES antes de qualquer input handler.
 *
 * @param idle_timeout_s   Segundos em IDLE antes de transitar para SLEEPING.
 *                         Configurável via NVS (NB_CFG_KEY_IDLE_TMO).
 * @param start_safe_mode  Se true, transita imediatamente para SAFE_MODE.
 * @param change_cb        Callback de transição. Pode ser NULL.
 * @return ESP_OK ou ESP_ERR_INVALID_STATE se já inicializado.
 */
esp_err_t state_machine_init(uint32_t              idle_timeout_s,
                              bool                  start_safe_mode,
                              nb_state_change_cb_t  change_cb);

/**
 * @brief Retorna o estado atual. Thread-safe.
 */
nb_robot_state_t state_machine_get_state(void);

/**
 * @brief Retorna o nome textual de um estado (string estática).
 */
const char *state_machine_state_name(nb_robot_state_t s);

/**
 * @brief Atualiza o idle timeout dinamicamente (sem reiniciar o timer atual).
 *
 * Usado pelo circadian_service para reduzir o timeout em fase DUSK.
 * Thread-safe.
 */
void state_machine_set_idle_timeout_s(uint32_t s);

/**
 * @brief Tick periódico — avança timers internos (idle timeout, touch timeout).
 *
 * Deve ser chamado a cada dt_ms ms pela behavior_task.
 * Thread-safe.
 *
 * @param dt_ms  Tempo decorrido desde a última chamada, em ms.
 */
void state_machine_update(uint32_t dt_ms);

/**
 * @brief Transita de BOOT_UP para IDLE. Chamado ao final de PHASE_COMPLETE.
 */
void state_machine_on_boot_complete(void);

/* ── Inputs de eventos externos ──────────────────────────────────────────── */
/* Chamados pelo boot_manager a partir de seus handlers de evento.            */

void state_machine_on_touch_tap(void);
void state_machine_on_touch_long_press(void);
void state_machine_on_touch_wake(void);
void state_machine_on_voice_start(void);
void state_machine_on_voice_end(void);
void state_machine_on_audio_started(void);
void state_machine_on_audio_ended(void);
void state_machine_on_motion_fault(void);

#endif /* NB_STATE_MACHINE_H */
