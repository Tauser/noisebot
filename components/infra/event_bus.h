/*
 * event_bus.h — Event bus do NoiseBot
 *
 * Pub/sub com pool estático (sem malloc por evento) e dois modos de entrega:
 *
 *   Síncrono  (nb_event_publish):       handlers chamados in-task, imediatamente.
 *   Assíncrono (nb_event_publish_async): evento enfileirado para a dispatcher task.
 *
 * Eventos de safety (NB_EVT_POWER_BROWNOUT_WARN, NB_EVT_MOTION_FAULT,
 * NB_EVT_MOTION_DISABLED) usam fila separada, sempre processada antes
 * da fila normal pelo dispatcher.
 *
 * Task dispatcher:
 *   Nome: "nb_event_task"   Stack: NB_EVENT_BUS_TASK_STACK
 *   Prioridade: NB_EVENT_BUS_TASK_PRIORITY   Core: qualquer
 *
 * Limites estáticos (sem alocação dinâmica):
 *   Pool     : NB_EVENT_BUS_POOL_SIZE eventos
 *   Fila     : NB_EVENT_BUS_QUEUE_SIZE entradas
 *   Safety   : NB_EVENT_BUS_SAFETY_QUEUE_SIZE entradas
 *   Subs/tipo: NB_EVENT_BUS_MAX_SUBS_PER_TYPE handlers
 */

#ifndef NB_EVENT_BUS_H
#define NB_EVENT_BUS_H

#include "nb_events.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Dimensionamento ──────────────────────────────────────────────────────── */

#define NB_EVENT_BUS_POOL_SIZE           32U  /**< Slots de evento assíncrono  */
#define NB_EVENT_BUS_QUEUE_SIZE          16U  /**< Capacidade da fila normal    */
#define NB_EVENT_BUS_SAFETY_QUEUE_SIZE    8U  /**< Capacidade da fila safety    */
#define NB_EVENT_BUS_MAX_SUBS_PER_TYPE    4U  /**< Subscribers por tipo         */
#define NB_EVENT_BUS_TASK_STACK        4096U  /**< Stack da dispatcher task     */
#define NB_EVENT_BUS_TASK_PRIORITY        8U  /**< Prioridade dispatcher (> 5)  */

/* ── Tipos ───────────────────────────────────────────────────────────────── */

/** Handle opaco retornado por nb_event_subscribe(). */
typedef uint32_t nb_event_sub_handle_t;
#define NB_EVENT_SUB_INVALID  0U

/**
 * Estrutura de evento.
 * Para publish síncrono: pode ser alocada na stack do caller.
 * Para publish assíncrono: copiada para slot do pool — o caller pode reutilizar
 * sua instância imediatamente após nb_event_publish_async() retornar.
 */
typedef struct {
    nb_event_type_t type;         /**< Tipo do evento                         */
    uint32_t        timestamp_ms; /**< esp_timer_get_time() / 1000 no publish  */
    union {
        uint32_t    u32;          /**< Payload genérico sem sinal              */
        int32_t     i32;          /**< Payload genérico com sinal              */
        float       f32;          /**< Payload ponto flutuante                 */
        void       *ptr;          /**< Ponteiro para dados externos (cuidado:  */
                                  /**<   lifetime deve cobrir a entrega async) */
        uint8_t     bytes[8];     /**< Payload raw até 8 bytes                 */
    } data;
} nb_event_t;

/** Protótipo do handler de evento. Chamado in-task pela dispatcher ou pelo caller. */
typedef void (*nb_event_handler_t)(const nb_event_t *evt, void *ctx);

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o event bus.
 *
 * Cria pool, filas, mutexes e a dispatcher task.
 * Deve ser chamado em PHASE_SERVICES após logger e watchdog iniciados.
 *
 * @return ESP_OK ou ESP_FAIL se já inicializado.
 */
esp_err_t nb_event_bus_init(void);

/**
 * @brief Registra um handler para um tipo de evento.
 *
 * Thread-safe. Máximo NB_EVENT_BUS_MAX_SUBS_PER_TYPE handlers por tipo.
 *
 * @param type       Tipo de evento a observar.
 * @param handler    Função a chamar na entrega.
 * @param ctx        Contexto opaco passado ao handler. Pode ser NULL.
 * @param out_handle Handle para unsubscribe. Pode ser NULL se não necessário.
 * @return ESP_OK, ESP_ERR_INVALID_ARG ou ESP_ERR_NO_MEM (slots cheios).
 */
esp_err_t nb_event_subscribe(nb_event_type_t        type,
                              nb_event_handler_t     handler,
                              void                  *ctx,
                              nb_event_sub_handle_t *out_handle);

/**
 * @brief Remove um subscriber pelo handle.
 *
 * Thread-safe. Handler não será mais chamado após retorno desta função.
 *
 * @return ESP_OK ou ESP_ERR_NOT_FOUND se handle inválido/já removido.
 */
esp_err_t nb_event_unsubscribe(nb_event_sub_handle_t handle);

/**
 * @brief Publica evento de forma síncrona.
 *
 * Chama todos os handlers registrados para evt->type in-task, em ordem de
 * registro. Bloqueia até todos os handlers retornarem.
 * Não usa pool — evt pode estar na stack do caller.
 *
 * @param evt Ponteiro para o evento. Não deve ser NULL.
 * @return ESP_OK ou ESP_ERR_INVALID_ARG.
 */
esp_err_t nb_event_publish(const nb_event_t *evt);

/**
 * @brief Publica evento de forma assíncrona (cross-task).
 *
 * Copia evt para um slot do pool e enfileira para a dispatcher task.
 * Eventos de safety vão para a fila prioritária.
 * Se pool ou fila cheios: incrementa dropped counter, retorna ESP_ERR_NO_MEM.
 *
 * @param evt Ponteiro para o evento. Não deve ser NULL.
 * @return ESP_OK ou ESP_ERR_NO_MEM (pool/fila cheios — evento descartado).
 */
esp_err_t nb_event_publish_async(const nb_event_t *evt);

/**
 * @brief Retorna o total acumulado de eventos descartados desde o boot.
 */
uint32_t nb_event_get_dropped_count(void);

#endif /* NB_EVENT_BUS_H */
