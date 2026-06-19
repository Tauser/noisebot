/*
 * camera_hal.h - HAL da camera DVP OV2640 (Layer 1).
 *
 * Bring-up minimo da camera DVP da placa Freenove ESP32-S3-WROOM CAM N16R8.
 * A camera captura frames para analise/diagnostico; nao desenha no display e
 * nao publica eventos diretamente.
 */

#ifndef NB_CAMERA_HAL_H
#define NB_CAMERA_HAL_H

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
} nb_camera_frame_t;

typedef enum {
    NB_CAMERA_MODE_SAFE_QQVGA = 0,  /**< 160x120: modo seguro padrao. */
    NB_CAMERA_MODE_BETTER_QVGA,     /**< 320x240: diagnostico experimental. */
} nb_camera_mode_t;

esp_err_t camera_hal_set_mode(nb_camera_mode_t mode);
nb_camera_mode_t camera_hal_get_mode(void);
const char *camera_hal_mode_name(nb_camera_mode_t mode);
/** @brief Returns a non-zero fallback for the Kconfig-fixed native resolution
 *         (mode does not change it). camera_hal_effective_*() below is the
 *         live, driver-read source of truth once a frame has been captured. */
size_t camera_hal_mode_width(nb_camera_mode_t mode);
size_t camera_hal_mode_height(nb_camera_mode_t mode);
/** @brief Returns the actual resolution reported by VIDIOC_G_FMT (0 before init). */
size_t camera_hal_effective_width(void);
size_t camera_hal_effective_height(void);
/** @brief Returns errno captured from the last failing VIDIOC_S_FMT, or 0 if none/success. */
int camera_hal_last_sfmt_errno(void);
size_t camera_hal_mode_min_dma_before(nb_camera_mode_t mode);
size_t camera_hal_mode_min_dma_largest(nb_camera_mode_t mode);
size_t camera_hal_mode_min_internal_before(nb_camera_mode_t mode);

/**
 * @brief Retorna true quando o firmware foi compilado com o driver real.
 *
 * Builds normais do NoiseBot habilitam o backend esp_video/V4L2. O retorno
 * false indica que a opcao de camera foi desligada explicitamente no sdkconfig.
 */
bool camera_hal_is_supported(void);

/**
 * @brief Inicializa a camera DVP via esp_video/V4L2.
 *
 * @return ESP_OK em sucesso. Falhas indicam camera ausente, driver indisponivel
 *         ou PSRAM abaixo do headroom minimo do projeto. SRAM/DMA baixa apos
 *         init gera warning/diagnostico, mas nao bloqueia a captura.
 */
esp_err_t camera_hal_init(void);

/**
 * @brief Verifica se a camera foi inicializada com sucesso.
 */
bool camera_hal_is_ready(void);

/**
 * @brief Captura um frame e o mantem emprestado ate camera_hal_release_frame().
 *
 * Apenas um frame pode ficar pendente por vez. Retorna ESP_ERR_INVALID_STATE se
 * a camera nao estiver pronta ou se ja houver frame pendente.
 */
esp_err_t camera_hal_capture(void);

/**
 * @brief Retorna o frame pendente capturado por camera_hal_capture().
 *
 * O ponteiro pertence ao buffer V4L2 mapeado. Nao liberar manualmente.
 */
nb_camera_frame_t *camera_hal_get_frame(void);

/**
 * @brief Libera o frame capturado de volta ao driver.
 */
void camera_hal_release_frame(void);

/**
 * @brief Desinicializa a camera e libera recursos do driver.
 */
void camera_hal_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_CAMERA_HAL_H */
