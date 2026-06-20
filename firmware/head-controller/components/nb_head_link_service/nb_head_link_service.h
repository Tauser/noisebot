#ifndef NB_HEAD_LINK_SERVICE_H
#define NB_HEAD_LINK_SERVICE_H

#include "esp_err.h"
#include "nb_camera_protocol.h"
#include "nb_link_engine.h"

/*
 * Retorna ESP_ERR_NOT_SUPPORTED sem tocar GPIO/SPI enquanto o enlace estiver
 * desabilitado por configuração.
 */
esp_err_t nb_head_link_service_init(void);
nb_link_state_t nb_head_link_service_state(void);

/*
 * Sink para nb_head_camera_service_init(): enfileira o evento para a task do
 * enlace enviar (o engine não é thread-safe entre tasks). Chamado pela task
 * dedicada da câmera, nunca pela task do enlace.
 */
esp_err_t nb_head_link_service_send_camera_event(
    const nb_camera_link_event_t *event);

#endif /* NB_HEAD_LINK_SERVICE_H */
