/*
 * presence_semantic_service.h — Presença social anônima (Layer 5)
 *
 * FSM de 7 estados que interpreta sinais brutos de visão (face box do servidor
 * + heurística local do vision_service) e publica eventos NB_EVT_SOCIAL_PRESENCE_*
 * no event bus. Nenhum dado de identidade; apenas "há alguém" ou "não há".
 *
 * Precedente arquitetural: touch_semantic_service (mesma separação sensor/semântica).
 * Offline-first: opera com heurística local quando bridge não está disponível.
 *
 * Task: sem task própria — tick chamado por behavior_task (100ms).
 */

#ifndef NB_PRESENCE_SEMANTIC_SERVICE_H
#define NB_PRESENCE_SEMANTIC_SERVICE_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PRESENCE_NO_ONE        = 0,
    PRESENCE_MAYBE_SOMEONE = 1,
    PRESENCE_PRESENT       = 2,
    PRESENCE_ENGAGED       = 3,
    PRESENCE_LEFT_RECENTLY = 4,
    PRESENCE_AWAY          = 5,
    PRESENCE_ALONE_SETTLED = 6,
} presence_state_t;

typedef struct {
    presence_state_t state;
    uint32_t         presence_session_s;   /**< segundos em PRESENT + ENGAGED nesta sessão */
    uint32_t         absence_s;            /**< segundos desde o último sinal              */
    int64_t          last_seen_at_ms;      /**< timestamp (ms) da última detecção          */
    bool             confidence_confirmed; /**< true = face box; false = só heurística     */
    uint32_t         presence_confirmed_count;
    uint32_t         presence_returned_count;
    uint32_t         presence_away_count;
} presence_diag_t;

/**
 * @brief Inicializa o serviço e subscreve os eventos necessários.
 * Chamar após event_bus_init() e antes do first tick.
 */
esp_err_t presence_semantic_init(void);

/**
 * @brief Tick periódico — avança FSM e timers de ausência.
 * Chamado pela behavior_task a cada dt_ms ms.
 */
void presence_semantic_tick(uint32_t dt_ms);

/**
 * @brief Retorna snapshot dos contadores de diagnóstico. Thread-safe.
 */
presence_diag_t presence_semantic_get_diag(void);

#ifdef __cplusplus
}
#endif
#endif /* NB_PRESENCE_SEMANTIC_SERVICE_H */
