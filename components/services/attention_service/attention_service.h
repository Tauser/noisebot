/*
 * attention_service.h — Modelo contínuo de atenção do NoiseBot (Layer 5)
 *
 * Mantém um nível de atenção [0.0, 1.0] com decaimento exponencial (τ=30s).
 * Estímulos externos (voz, toque, sons) elevam o nível imediatamente.
 *
 * Consumidores:
 *   gaze_service  — velocidade de saccade ∝ attention_level
 *   idle_service  — intervalo de yawn ∝ (1 - attention_level)
 *
 * Tick: chamado de behavior_task (100ms) via attention_service_tick().
 * Estímulos: chegam via event bus (subscrito internamente) ou chamada direta.
 */

#ifndef NB_ATTENTION_SERVICE_H
#define NB_ATTENTION_SERVICE_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Fontes de estímulo ──────────────────────────────────────────────────── */

typedef enum {
    NB_ATTN_VOICE_START  = 0, /**< Início de atividade de voz   (+0.70) */
    NB_ATTN_TOUCH_TAP    = 1, /**< Toque tap                     (+0.50) */
    NB_ATTN_SOUND_CLAP   = 2, /**< Palmas detectadas             (+0.40) */
    NB_ATTN_SOUND_LOUD   = 3, /**< RMS alto durante voz          (+0.20) */
    NB_ATTN_SOUND_MUSIC  = 4, /**< Música ambiente iniciou       (+0.15) */
    NB_ATTN_SOURCE_COUNT,
} nb_attention_source_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o serviço e subscreve eventos do event bus.
 *
 * @return ESP_OK ou ESP_ERR_INVALID_STATE se já inicializado.
 */
esp_err_t attention_service_init(void);

/**
 * @brief Injeta um estímulo externo diretamente (sem event bus).
 *
 * Thread-safe. Uso alternativo: o serviço já subscreve os eventos
 * principais em attention_service_init().
 *
 * @param src       Fonte do estímulo.
 * @param intensity Multiplicador [0.0, 1.0] aplicado ao peso da fonte.
 */
void attention_service_on_stimulus(nb_attention_source_t src, float intensity);

/**
 * @brief Retorna o nível de atenção atual [0.0, 1.0].
 *
 * Thread-safe (portMUX spinlock).
 */
float attention_service_get_level(void);

/**
 * @brief Avança o decaimento exponencial e verifica SOUND_LOUD via RMS.
 *
 * Chamado de behavior_task a cada dt_ms.
 *
 * @param dt_ms Intervalo desde a última chamada, em ms.
 */
void attention_service_tick(uint32_t dt_ms);

#ifdef __cplusplus
}
#endif

#endif /* NB_ATTENTION_SERVICE_H */
