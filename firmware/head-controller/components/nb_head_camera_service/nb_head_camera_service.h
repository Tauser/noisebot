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

typedef esp_err_t (*nb_head_camera_event_sink_fn)(
    const nb_camera_link_event_t *event);

/*
 * sink é chamado pela task dedicada da câmera (nunca pela task do enlace)
 * sempre que um comando é processado, com o evento de resposta já pronto.
 */
esp_err_t nb_head_camera_service_init(nb_head_camera_event_sink_fn sink);

/*
 * Valida o comando e enfileira para a task dedicada da câmera — nunca
 * bloqueia quem chama. A captura V4L2 (potencialmente lenta) acontece fora
 * da task do enlace; ver docs/DM4_BRINGUP.md sobre por que isso é
 * obrigatório (captura síncrona na task do enlace atrasa ACK/heartbeat).
 */
esp_err_t nb_head_camera_service_apply(const void *payload, uint16_t length);
void nb_head_camera_service_get_status(nb_head_camera_status_t *out);

#endif /* NB_HEAD_CAMERA_SERVICE_H */
