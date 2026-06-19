/*
 * schedule_service.h — Timer de um disparo e recorrente (Layer 5)
 *
 * Fornece scheduling tickless integrado ao behavior_task: sem task própria,
 * sem malloc, sem FreeRTOS timers. Tudo resolvido via schedule_service_tick()
 * chamado pelo behavior_task a cada iteração do loop.
 *
 * Slots estáticos: NB_SCHED_MAX_SLOTS (16). Tentativa de alocar além do limite
 * retorna NB_SCHED_INVALID_HANDLE sem assert fatal — o chamador deve checar.
 *
 * Thread-safety: schedule_after(), schedule_repeat() e schedule_cancel() podem
 * ser chamados de qualquer task; schedule_service_tick() deve ser chamado de
 * uma única task (behavior_task).
 */

#ifndef NB_SCHEDULE_SERVICE_H
#define NB_SCHEDULE_SERVICE_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Capacidade ──────────────────────────────────────────────────────────── */

#define NB_SCHED_MAX_SLOTS  16U   /**< Número máximo de schedules simultâneos. */

/* ── Handle ──────────────────────────────────────────────────────────────── */

typedef uint8_t schedule_handle_t;

/** Handle inválido / sem slot disponível. */
#define NB_SCHED_INVALID_HANDLE  ((schedule_handle_t)0xFF)

/* ── Callback ────────────────────────────────────────────────────────────── */

/**
 * Callback disparado quando o schedule vence.
 * @param ctx  Ponteiro de contexto passado em schedule_after() / schedule_repeat().
 */
typedef void (*schedule_cb_t)(void *ctx);

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o schedule_service.
 *
 * Deve ser chamado uma única vez antes de schedule_service_tick().
 *
 * @return ESP_OK ou ESP_ERR_INVALID_STATE se já inicializado.
 */
esp_err_t schedule_service_init(void);

/**
 * @brief Avança todos os timers ativos.
 *
 * Deve ser chamado pelo behavior_task em cada iteração, passando o tempo
 * decorrido desde a última chamada em milissegundos.
 * Callbacks são invocados inline nesta chamada.
 * Não chamar de ISR nem de tasks de safety.
 *
 * @param dt_ms  Tempo decorrido em ms desde o último tick.
 */
void schedule_service_tick(uint32_t dt_ms);

/**
 * @brief Agenda um callback único após @p delay_ms milissegundos.
 *
 * O slot é liberado automaticamente após o disparo.
 *
 * @param delay_ms  Atraso em ms. Zero dispara no próximo tick.
 * @param cb        Callback a invocar. Não pode ser NULL.
 * @param ctx       Ponteiro passado ao callback. Pode ser NULL.
 * @return Handle do schedule, ou NB_SCHED_INVALID_HANDLE se sem slots.
 */
schedule_handle_t schedule_after(uint32_t delay_ms, schedule_cb_t cb, void *ctx);

/**
 * @brief Agenda um callback recorrente com intervalo @p interval_ms.
 *
 * O slot persiste até schedule_cancel() ser chamado.
 *
 * @param interval_ms  Intervalo em ms. Deve ser > 0.
 * @param cb           Callback a invocar. Não pode ser NULL.
 * @param ctx          Ponteiro passado ao callback. Pode ser NULL.
 * @return Handle do schedule, ou NB_SCHED_INVALID_HANDLE se sem slots.
 */
schedule_handle_t schedule_repeat(uint32_t interval_ms, schedule_cb_t cb, void *ctx);

/**
 * @brief Cancela um schedule pelo handle.
 *
 * No-op silencioso se o handle for NB_SCHED_INVALID_HANDLE ou já expirado.
 * Thread-safe: pode ser chamado de qualquer task.
 *
 * @param h  Handle retornado por schedule_after() ou schedule_repeat().
 */
void schedule_cancel(schedule_handle_t h);

#ifdef __cplusplus
}
#endif

#endif /* NB_SCHEDULE_SERVICE_H */
