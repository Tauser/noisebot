/*
 * behavior_engine.h — Motor de regras comportamentais do NoiseBot (Layer 6)
 *
 * Tabela de regras declarativa avaliada a cada evento do event bus.
 * Elimina os switch/case de comportamento do boot_manager — todas as reações
 * a eventos são expressas como regras estáticas neste componente.
 *
 * A tabela de regras é interna ao .c (sem malloc). A API pública é apenas
 * behavior_engine_init(), chamado pelo boot_manager em PHASE_SERVICES.
 *
 * Prioridade: regra com condition_fn vence fallback (cond = NULL) para o
 * mesmo trigger. Múltiplas regras com conditions não sobrepostas são
 * todas avaliadas (ex: STATE_CHANGED → sleeping, waking, error).
 *
 * Dependências:
 *   Layer 2 — infra (event_bus)
 *   Layer 5 — conductor, idle_service
 *   Layer 6 — state_machine, emotion_model
 *   Layer 7 — long_term_memory  (exceção arquitetural — LTM é comportamental)
 */

#ifndef NB_BEHAVIOR_ENGINE_H
#define NB_BEHAVIOR_ENGINE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa o behavior_engine e registra subscrições no event bus.
 *
 * Deve ser chamado após todos os serviços de Layer 4–6 e o event_bus
 * estarem inicializados. Chamado pelo boot_manager em PHASE_SERVICES.
 *
 * @return ESP_OK ou ESP_ERR_INVALID_STATE se já inicializado.
 */
esp_err_t behavior_engine_init(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_BEHAVIOR_ENGINE_H */
