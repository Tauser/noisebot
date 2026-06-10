/*
 * vision_preview_service.h — Preview da câmera no display com overlay de face (Layer 4).
 *
 * Funcionalidade:
 *   - Registra uma render layer de z=90 que exibe o JPEG mais recente da câmera.
 *   - Subscreve NB_EVT_BRIDGE_FACE_BOX para sobrepor retângulo de detecção.
 *   - Loop interno de captura a ~5 FPS em task dedicada (Core 0, Prio 3).
 *   - Habilitado/desabilitado em runtime sem afetar outros layers.
 *
 * Task: "nb_vpreview_task"  Core 0  Prio 3  Stack 6KB
 */

#ifndef NB_VISION_PREVIEW_SERVICE_H
#define NB_VISION_PREVIEW_SERVICE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_VISION_PREVIEW_LAYER_Z   90U
#define NB_VISION_PREVIEW_TASK_STACK  6144U
#define NB_VISION_PREVIEW_TASK_PRIO   3U
#define NB_VISION_PREVIEW_TASK_CORE   0
#define NB_VISION_PREVIEW_FPS         5U      /* capturas por segundo */
#define NB_VISION_PREVIEW_JPEG_MAX    (80 * 1024U)  /* max JPEG PSRAM buffer */
#define NB_VISION_PREVIEW_BOX_COLOR   0x07E0U  /* verde RGB565 */
#define NB_VISION_PREVIEW_BOX_TTL_MS  3000U   /* bbox expira após 3s sem update */

/**
 * @brief Inicializa e registra o preview service.
 *
 * Aloca buffer JPEG em PSRAM, registra render layer, subscreve eventos.
 * Requer camera_service_init() e render_service_init() anteriores.
 *
 * @return ESP_OK ou ESP_ERR_NO_MEM.
 */
esp_err_t vision_preview_service_init(void);

/**
 * @brief Inicia o loop de captura.
 */
esp_err_t vision_preview_service_start(void);

/**
 * @brief Para o loop de captura (layer continua registrada mas em branco).
 */
void vision_preview_service_stop(void);

/**
 * @brief Habilita ou desabilita a renderização do preview na tela.
 *
 * Thread-safe. Quando desabilitado, a layer não desenha nada (transparente).
 */
void vision_preview_service_set_enabled(bool enabled);

bool vision_preview_service_is_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_VISION_PREVIEW_SERVICE_H */
