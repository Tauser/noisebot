#ifndef NB_HEAD_CAMERA_SERVICE_H
#define NB_HEAD_CAMERA_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "nb_camera_protocol.h"

typedef struct {
    bool enabled;
    bool hardware_ready;
    uint32_t accepted;
    uint32_t rejected;
} nb_head_camera_status_t;

esp_err_t nb_head_camera_service_init(void);

/*
 * Valida o comando e preenche *out_event com a resposta semântica. Quando
 * CONFIG_NB_HEAD_CAMERA_HW_ENABLED está desligado (ou o driver DVP falhou
 * ao iniciar), hardware_ready permanece falso e o evento sempre responde
 * NB_CAMERA_LINK_STATUS_UNAVAILABLE. Com hardware pronto e opcode
 * REQUEST_SNAPSHOT, tenta capturar e responde OK/ERROR com as dimensões e o
 * tamanho do frame — nunca os pixels (isso é trabalho do canal BULK futuro).
 */
esp_err_t nb_head_camera_service_apply(const void *payload, uint16_t length,
                                       nb_camera_link_event_t *out_event);
void nb_head_camera_service_get_status(nb_head_camera_status_t *out);

#endif /* NB_HEAD_CAMERA_SERVICE_H */
