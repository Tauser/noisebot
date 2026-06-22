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

/*
 * DM2.11 (fatia minima) -- atualiza o bitmask de icones de status (so
 * marca dirty; quem desenha de fato e' a propria display_task no proximo
 * tick, nunca a thread chamadora). Chamado por nb_head_link_service ao
 * receber NB_LINK_MSG_DISPLAY_STATUS_ICONS_V2 -- nao bloquear, nao
 * desenhar aqui, mesmo cuidado que evitou esfomear o watchdog no DM2.9.
 */
esp_err_t nb_head_display_service_set_status_icons(uint32_t icon_bits);

/*
 * DM2.12 -- stack high-water-mark da display_task, em palavras (words),
 * pra acompanhar headroom de stack agora que ela faz blend/blink/icones
 * todo tick. Retorna 0 se a task ainda nao foi criada.
 */
uint32_t nb_head_display_service_get_stack_min_free_words(void);

#endif /* NB_HEAD_DISPLAY_SERVICE_H */
