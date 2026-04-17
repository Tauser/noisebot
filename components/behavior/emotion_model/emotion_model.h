/*
 * emotion_model.h — Modelo emocional do NoiseBot (Layer 6)
 *
 * Mantém um vetor emocional (valência, ativação) em [-1, 1].
 *
 *   valência:  negativo = aversão/desprazer, positivo = prazer/calor
 *   ativação:  negativo = calmo/sonolento,   positivo = excitado/alerta
 *
 * Dinâmica:
 *   - Eventos externos ajustam o vetor via emotion_model_on_event().
 *   - A cada tick (emotion_model_update), o vetor decai exponencialmente
 *     em direção a (0, 0) — neutral. Após ~60s decai a <5% do pico.
 *   - O vetor é mapeado por nearest-neighbor a uma das 9 expressões base.
 *   - Mudanças de expressão chamam expression_service_set() com transição suave.
 *
 * Não depende de infra diretamente. Callbacks permitem ao boot_manager
 * persistir a emoção no NVS e publicar eventos no event bus.
 *
 * Thread-safe via portMUX_TYPE spinlock.
 */

#ifndef NB_EMOTION_MODEL_H
#define NB_EMOTION_MODEL_H

#include "esp_err.h"
#include "expression_service.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Vetor emocional ─────────────────────────────────────────────────────── */

typedef struct {
    float valence;     /**< [-1, 1]  negativo → positivo                    */
    float activation;  /**< [-1, 1]  calmo    → excitado                    */
} nb_emotion_vec_t;

/* ── Eventos que modificam a emoção ─────────────────────────────────────── */

typedef enum {
    NB_EMOT_EVT_TOUCH_TAP       = 0,  /**< Toque breve     → calor + pequena excitação  */
    NB_EMOT_EVT_TOUCH_LONG      = 1,  /**< Toque longo     → calor maior                */
    NB_EMOT_EVT_VOICE_START     = 2,  /**< Voz detectada   → atenção + alerta            */
    NB_EMOT_EVT_AUDIO_STARTED   = 3,  /**< Áudio em play   → engajamento                 */
    NB_EMOT_EVT_ENTERING_SLEEP  = 4,  /**< Indo dormir     → calmo / sonolento            */
    NB_EMOT_EVT_WAKING_UP       = 5,  /**< Acordando       → leve despertar               */
    NB_EMOT_EVT_MOTION_FAULT    = 6,  /**< Falha de motion → alarme                       */
    NB_EMOT_EVT_IDLE_LONG       = 7,  /**< Sozinho por muito tempo → tristeza suave        */
    NB_EMOT_EVT_VOICE_LOUD      = 8,  /**< Fala em tom alto → alarme/surpresa              */
    NB_EMOT_EVT_VOICE_SOFT      = 9,  /**< Fala suave → curiosidade atenta                 */
    NB_EMOT_EVT_COUNT           = 10,
} nb_emotion_event_t;

/* ── Callback ────────────────────────────────────────────────────────────── */

/**
 * Chamado quando a expressão discreta muda.
 * Permite ao boot_manager persistir via config_set_last_emotion().
 */
typedef void (*nb_emotion_change_cb_t)(nb_expression_t new_expr);

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o modelo emocional.
 *
 * O vetor é inicializado no anchor da expressão inicial.
 * Deve ser chamado após expression_service_init().
 *
 * @param initial_expr  Expressão de partida (lida do NVS ou NB_EXPR_NEUTRAL).
 * @param change_cb     Callback quando a expressão mapeada muda. Pode ser NULL.
 * @return ESP_OK ou ESP_ERR_INVALID_STATE se já inicializado.
 */
esp_err_t emotion_model_init(nb_expression_t         initial_expr,
                              nb_emotion_change_cb_t  change_cb);

/**
 * @brief Tick periódico — decaimento em direção a neutral e re-mapeamento.
 *
 * Deve ser chamado a cada dt_ms ms pela behavior_task.
 * Thread-safe.
 */
void emotion_model_update(uint32_t dt_ms);

/**
 * @brief Responde a um evento ajustando o vetor emocional.
 *
 * Aplica delta (Δv, Δa) predefinido para o evento, clampeia em [-1, 1],
 * e re-avalia a expressão imediatamente.
 * Thread-safe.
 */
void emotion_model_on_event(nb_emotion_event_t evt);

/** Retorna a expressão discreta atual. Thread-safe. */
nb_expression_t  emotion_model_get_expression(void);

/** Retorna o vetor emocional atual. Thread-safe. */
nb_emotion_vec_t emotion_model_get_vec(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_EMOTION_MODEL_H */
