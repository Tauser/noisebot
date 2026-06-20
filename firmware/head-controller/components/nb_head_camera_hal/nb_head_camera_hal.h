/*
 * nb_head_camera_hal.h — HAL da câmera DVP OV2640 do head (Layer 1)
 *
 * Bring-up físico da câmera DVP da Freenove ESP32-S3-WROOM CAM N16R8.
 * Porte de camera_hal.c (main-controller legado) para o head, único dono
 * físico dos pinos DVP. Backend esp_video/V4L2, igual ao original.
 */

#ifndef NB_HEAD_CAMERA_HAL_H
#define NB_HEAD_CAMERA_HAL_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t width;
    size_t height;
    int format;
} nb_head_camera_frame_t;

esp_err_t nb_head_camera_hal_init(void);
bool nb_head_camera_hal_is_ready(void);

/*
 * Captura um frame e o mantém emprestado até
 * nb_head_camera_hal_release_frame(). Apenas um frame pendente por vez.
 */
esp_err_t nb_head_camera_hal_capture(void);
nb_head_camera_frame_t *nb_head_camera_hal_get_frame(void);
void nb_head_camera_hal_release_frame(void);
void nb_head_camera_hal_deinit(void);

size_t nb_head_camera_hal_effective_width(void);
size_t nb_head_camera_hal_effective_height(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_HEAD_CAMERA_HAL_H */
