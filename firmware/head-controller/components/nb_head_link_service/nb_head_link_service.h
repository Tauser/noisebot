#ifndef NB_HEAD_LINK_SERVICE_H
#define NB_HEAD_LINK_SERVICE_H

#include "esp_err.h"
#include "nb_link_engine.h"

/*
 * Retorna ESP_ERR_NOT_SUPPORTED sem tocar GPIO/SPI enquanto o enlace estiver
 * desabilitado por configuração.
 */
esp_err_t nb_head_link_service_init(void);
nb_link_state_t nb_head_link_service_state(void);

#endif /* NB_HEAD_LINK_SERVICE_H */
