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

#ifdef __cplusplus
}
#endif

#endif /* NB_VISUAL_STATE_FACADE_H */
