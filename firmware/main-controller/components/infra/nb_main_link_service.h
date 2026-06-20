#ifndef NB_MAIN_LINK_SERVICE_H
#define NB_MAIN_LINK_SERVICE_H

#include "esp_err.h"
#include "nb_camera_protocol.h"
#include "nb_display_protocol.h"
#include "nb_link_engine.h"

/*
 * Retorna ESP_ERR_NOT_SUPPORTED sem tocar GPIO/SPI enquanto o enlace estiver
 * desabilitado por configuração.
 */
esp_err_t nb_main_link_service_init(void);
nb_link_state_t nb_main_link_service_state(void);
esp_err_t nb_main_link_service_reset_head(void);
esp_err_t nb_main_link_service_queue_display(
    const nb_display_command_t *command);

/*
 * Envia um comando de câmera ao head sob demanda (fire-once, sem fila de
 * estado persistente como o display). Falha com ESP_ERR_INVALID_STATE fora
 * de READY e com ESP_ERR_NOT_SUPPORTED se o head não anunciou
 * NB_LINK_CAP_CAMERA_SEMANTIC.
 */
esp_err_t nb_main_link_service_request_camera(
    const nb_camera_link_command_t *command);

/*
 * Copia o último nb_camera_link_event_t recebido do head. Retorna
 * ESP_ERR_NOT_FOUND se nenhum evento chegou ainda.
 */
esp_err_t nb_main_link_service_get_last_camera_event(
    nb_camera_link_event_t *out_event);

#endif /* NB_MAIN_LINK_SERVICE_H */
