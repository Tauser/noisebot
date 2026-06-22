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

/*
 * DM2.8 -- igual a nb_head_display_hal_apply, mas desenha a face
 * interpolada entre o estado atualmente exibido e o alvo (`target`), na
 * fracao `face_t` ([0..1]). gaze/overlay/brilho do alvo sao aplicados
 * direto, sem interpolacao -- só a geometria da face faz blend. Chamar
 * repetidamente com `face_t` crescente produz a transicao suave; em
 * `face_t >= 1.0f` finaliza exatamente no valor da tabela (sem drift).
 */
esp_err_t nb_head_display_hal_apply_blend(const nb_display_command_t *target,
                                          float face_t);

bool nb_head_display_hal_is_ready(void);
uint32_t nb_head_display_hal_spiram_free(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_HEAD_DISPLAY_HAL_H */
