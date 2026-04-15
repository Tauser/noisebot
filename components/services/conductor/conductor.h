/*
 * conductor.h — Orquestrador de ações de alto nível do NoiseBot (Layer 5)
 *
 * Executa "partituras" com keyframes temporais que coordenam face, motion
 * e áudio como uma única expressão unificada. Cada ação tem 2–3 variações
 * sorteadas aleatoriamente via hardware RNG.
 *
 * Interrupt suave: nova ação durante ação em curso → para áudio, para motion
 * suavemente e inicia a nova ação. Não há tearing de expressão facial.
 *
 * Task: "nb_conductor_task"  Core: 0  Prio: 6  Stack: 4096
 */

#ifndef NB_CONDUCTOR_H
#define NB_CONDUCTOR_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Ações de alto nível ─────────────────────────────────────────────────── */

typedef enum {
    NB_ACTION_NONE          = 0,
    NB_ACTION_GREET         = 1,   /**< Saudação (boot / encontro)                */
    NB_ACTION_AGREE         = 2,   /**< Concordar / affirmar                      */
    NB_ACTION_DISAGREE      = 3,   /**< Discordar / negar                         */
    NB_ACTION_CURIOUS       = 4,   /**< Demonstrar curiosidade                    */
    NB_ACTION_TOUCH_WARM    = 5,   /**< Resposta calorosa a toque (TAP)           */
    NB_ACTION_TOUCH_STARTLE = 6,   /**< Sobressalto a toque longo                 */
    NB_ACTION_SPEAK_LOOP    = 7,   /**< Modo de fala ativa (durante playback)     */
    NB_ACTION_SLEEP         = 8,   /**< Entrar em modo dormindo                   */
    NB_ACTION_WAKE_UP       = 9,   /**< Despertar                                 */
    NB_ACTION_COUNT         = 10,
} nb_action_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o conductor e cria a task.
 * Deve ser chamado após todos os serviços de Layer 4–5 inicializados.
 */
esp_err_t conductor_init(void);

/**
 * @brief Dispara uma ação. Thread-safe.
 * Se uma ação estiver em curso, é interrompida suavemente e a nova inicia.
 * NB_ACTION_NONE para apenas interromper a ação atual.
 */
void conductor_play(nb_action_t action);

/** @brief Retorna a ação em curso (NB_ACTION_NONE se idle). Thread-safe. */
nb_action_t conductor_get_current(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_CONDUCTOR_H */
