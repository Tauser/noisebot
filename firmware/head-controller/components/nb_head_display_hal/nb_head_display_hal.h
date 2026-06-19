#ifndef NB_HEAD_DISPLAY_HAL_H
#define NB_HEAD_DISPLAY_HAL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "nb_display_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nb_head_display_hal_init(void);
esp_err_t nb_head_display_hal_apply(const nb_display_command_t *command);
bool nb_head_display_hal_is_ready(void);
uint32_t nb_head_display_hal_spiram_free(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_HEAD_DISPLAY_HAL_H */
