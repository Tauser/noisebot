#ifndef NB_VISUAL_STATE_FACADE_H
#define NB_VISUAL_STATE_FACADE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "nb_display_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*nb_visual_state_sink_fn)(
    const nb_display_command_t *command);

esp_err_t visual_state_facade_init(nb_visual_state_sink_fn sink);
void visual_state_facade_set_expression(uint8_t expression);
void visual_state_facade_set_gaze(float x, float y);
void visual_state_facade_set_brightness(uint8_t brightness);
void visual_state_facade_set_overlay(uint16_t flag, bool enabled);
void visual_state_facade_force_publish(void);

/*
 * DM2.11 -- ponte neutra pra status icons, mesmo motivo do sink de scene:
 * ui_overlay_service (que chama set_status_icons) não pode depender de
 * nb_main_link_service.h sem criar dependência circular de componente
 * (infra já depende de ui_overlay_service). boot_manager.c registra o
 * sink real uma vez no boot; sem sink registrado, a chamada é só um
 * no-op local (sem head, sem link, nada quebra).
 */
typedef esp_err_t (*nb_visual_status_icons_sink_fn)(uint32_t icon_bits);
void visual_state_facade_set_status_icons_sink(
    nb_visual_status_icons_sink_fn sink);
void visual_state_facade_set_status_icons(uint32_t icon_bits);

#ifdef __cplusplus
}
#endif

#endif /* NB_VISUAL_STATE_FACADE_H */
