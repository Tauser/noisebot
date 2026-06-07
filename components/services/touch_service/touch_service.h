/*
 * touch_service.h — Serviço de touch do NoiseBot (Layer 4)
 *
 * Detecção robusta de toque com:
 *   - Histerese on/off para evitar chatter perto do threshold
 *   - Debounce de release para evitar ruído
 *   - Recalibração lenta de baseline (drift ambiental)
 *   - Proteção contra baseline poisoning durante toque ativo
 *   - Timeout de estabilização após boot
 *
 * Eventos (one-shot via callback):
 *   TAP          — primeiro toque detectado (<20ms de latência)
 *   LONG_PRESS   — press contínuo ≥ 1500ms
 *   SUSTAINED    — press contínuo ≥ 3000ms
 *   WAKE         — qualquer toque quando sleeping=true
 *
 * Estado contínuo:
 *   touch_service_is_pressed()         — true enquanto tocado
 *   touch_service_get_duration_ms()    — duração do toque atual
 *
 * Debug:
 *   touch_service_get_debug()          — raw, baseline, thresholds, state
 *
 * Threshold:
 *   threshold_on  = baseline × (1 + sensitivity)
 *   threshold_off = baseline × (1 + sensitivity × HYSTERESIS_FACTOR)
 *   HYSTERESIS_FACTOR = 0.4 (threshold_off é 40% do span de sensitivity)
 *
 * Thread safety:
 *   touch_service_update() deve ser chamado de uma única task.
 *   Demais funções são seguras de qualquer task (mutex interno).
 */

#ifndef NB_TOUCH_SERVICE_H
#define NB_TOUCH_SERVICE_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Tipos de evento ─────────────────────────────────────────────────────── */

typedef enum {
    NB_TOUCH_EVT_TAP = 0,
    NB_TOUCH_EVT_LONG_PRESS,
    NB_TOUCH_EVT_SUSTAINED,
    NB_TOUCH_EVT_WAKE,
} nb_touch_event_t;

/** Callback executado no contexto de touch_service_update(). Não bloquear. */
typedef void (*nb_touch_event_cb_t)(nb_touch_event_t event);

/* ── Estado interno exposto para debug ──────────────────────────────────── */

typedef enum {
    NB_TOUCH_STATE_IDLE = 0,
    NB_TOUCH_STATE_TOUCHING,
    NB_TOUCH_STATE_LONG_PRESSING,
    NB_TOUCH_STATE_SUSTAINED_ACTIVE,
} nb_touch_state_t;

typedef struct {
    uint32_t          raw;             /**< Leitura bruta do HAL               */
    uint32_t          filtered_raw;    /**< raw após EMA (usado pela FSM)      */
    uint32_t          baseline;        /**< Baseline dinâmico atual            */
    uint32_t          threshold_on;    /**< Threshold de detecção de toque     */
    uint32_t          threshold_off;   /**< Threshold de liberação (histerese) */
    nb_touch_state_t  state;           /**< Estado atual do serviço            */
    uint32_t          touch_duration_ms; /**< Duração do toque atual (ms)      */
} nb_touch_debug_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o serviço (chama touch_hal_init internamente).
 *
 * Deve ser chamado em PHASE_HAL. Sensitivity padrão: 0.20.
 * Os primeiros 1000ms após init são período de estabilização — nenhum evento
 * é disparado durante este período.
 *
 * @return ESP_OK em sucesso.
 */
esp_err_t touch_service_init(void);

/**
 * @brief Avança detecção em dt_ms e dispara callbacks se necessário.
 *
 * Chamar a cada 20ms (50Hz). Não bloqueante.
 * A confirmação de toque exige 3 amostras consecutivas (~60ms).
 */
void touch_service_update(uint32_t dt_ms);

/**
 * @brief Registra callback para eventos one-shot de touch.
 */
void touch_service_set_event_cb(nb_touch_event_cb_t cb);

/**
 * @brief Ajusta o fator de sensibilidade [0.002, 1.0].
 *
 * threshold_on  = baseline × (1 + factor)
 * threshold_off = baseline × (1 + factor × 0.4)
 */
void touch_service_set_sensitivity(float factor);

/**
 * @brief Informa modo sleeping. Quando true, touch dispara WAKE em vez de TAP.
 */
void touch_service_set_sleeping(bool sleeping);

/**
 * @brief Retorna true se o sensor está atualmente pressionado.
 */
bool touch_service_is_pressed(void);

/**
 * @brief Retorna duração do toque atual em ms (0 se não pressionado).
 */
uint32_t touch_service_get_duration_ms(void);

/**
 * @brief Preenche debug_out com métricas internas atuais.
 *
 * Thread-safe. Útil para calibração e diagnóstico em campo.
 */
void touch_service_get_debug(nb_touch_debug_t *debug_out);

#endif /* NB_TOUCH_SERVICE_H */
