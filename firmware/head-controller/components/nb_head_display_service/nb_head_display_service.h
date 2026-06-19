#ifndef NB_HEAD_DISPLAY_SERVICE_H
#define NB_HEAD_DISPLAY_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "nb_display_protocol.h"

typedef struct {
    bool enabled;
    bool scene_valid;
    uint32_t accepted;
    uint32_t rejected;
    uint32_t ignored;
    uint32_t hardware_errors;
    uint32_t spiram_free_bytes;
    bool hardware_ready;
    nb_display_command_t scene;
} nb_head_display_status_t;

esp_err_t nb_head_display_service_init(void);
esp_err_t nb_head_display_service_apply(const void *payload, uint16_t length);
void nb_head_display_service_get_status(nb_head_display_status_t *out);

#endif /* NB_HEAD_DISPLAY_SERVICE_H */
