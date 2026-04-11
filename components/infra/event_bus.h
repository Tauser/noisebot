#pragma once

#include "nb_events.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EVENT_BUS_MAX_SUBSCRIBERS   16
#define EVENT_BUS_PUBLISH_TIMEOUT_MS 10

/*
 * event_bus_init() — Inicializa estruturas internas e mutex.
 * Chamado no BOOT_PHASE_1.
 */
esp_err_t event_bus_init(void);

/*
 * event_bus_publish() — Entrega evento a todos os subscribers registrados.
 * Nao bloqueia por mais de EVENT_BUS_PUBLISH_TIMEOUT_MS.
 * NAO chamar de ISR context.
 */
esp_err_t event_bus_publish(const nb_event_t *evt);

/*
 * event_bus_subscribe() — Registra uma queue FreeRTOS para receber
 * eventos do tipo especificado. A queue deve ser criada pelo subscriber.
 * Um mesmo subscriber pode registrar multiplos tipos chamando N vezes.
 */
esp_err_t event_bus_subscribe(nb_event_type_t type, QueueHandle_t queue);

/*
 * event_bus_unsubscribe() — Remove registro.
 */
esp_err_t event_bus_unsubscribe(nb_event_type_t type, QueueHandle_t queue);

/*
 * Macro auxiliar para publicar evento simples sem payload.
 */
#define EVENT_BUS_PUBLISH_SIMPLE(evt_type, src_tag)                 \
    do {                                                            \
        nb_event_t _e = {                                           \
            .type         = (evt_type),                             \
            .timestamp_ms = pdTICKS_TO_MS(xTaskGetTickCount()),     \
            .source       = (src_tag),                              \
        };                                                          \
        event_bus_publish(&_e);                                     \
    } while (0)

#ifdef __cplusplus
}
#endif
