/*
 * camera_service.h - Servico de camera do NoiseBot (Layer 4).
 *
 * Coordena acesso ao camera_hal para snapshots sob demanda. Nao executa loop
 * continuo de captura para preservar CPU, PSRAM e o baseline visual do robo.
 */

#ifndef NB_CAMERA_SERVICE_H
#define NB_CAMERA_SERVICE_H

#include "esp_err.h"
#include "camera_hal.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t width;
    size_t height;
    int format;
} nb_camera_snapshot_t;

/**
 * @brief Encode quality intent for a JPEG capture.
 *
 * Resolution is native/effective (driver-fixed); this only selects the JPEG
 * encoder quality tradeoff for short camera captures.
 */
typedef enum {
    NB_CAMERA_QUALITY_SNAPSHOT = 0, /**< High detail — on-demand snapshot/analyze. */
} nb_camera_quality_t;

typedef struct {
    bool valid;
    uint32_t timestamp_ms;
    size_t width;
    size_t height;
    uint8_t luma_avg;
    uint8_t luma_min;
    uint8_t luma_max;
    uint8_t contrast;
    uint8_t motion_score;
    uint8_t spatial_score;
} nb_camera_scene_metrics_t;

typedef struct {
    bool valid;
    uint32_t capture_ms;
    size_t frame_bytes;
    int format;
    nb_camera_scene_metrics_t scene;
} nb_camera_observation_t;

typedef struct {
    size_t dma_free;
    size_t dma_largest;
    size_t internal_free;
    size_t psram_free;
    size_t min_dma_before;
    size_t min_dma_largest;
    size_t min_internal_before;
    nb_camera_mode_t mode;
    const char *mode_name;
    size_t mode_width;
    size_t mode_height;
    size_t effective_width;
    size_t effective_height;
    int last_sfmt_errno;
    size_t last_frame_bytes;
    size_t last_frame_width;
    size_t last_frame_height;
    int last_frame_format;
    size_t last_jpeg_bytes;
    uint32_t last_capture_ms;
    uint32_t capture_count;
    uint32_t fail_count;
    size_t last_dma_before;
    size_t last_dma_after_capture;
    size_t last_dma_after_release;
    size_t last_dma_largest_before;
    size_t last_dma_largest_after_capture;
    size_t last_dma_largest_after_release;
    size_t last_internal_before;
    size_t last_internal_after_capture;
    size_t last_internal_after_release;
    size_t last_psram_before;
    size_t last_psram_after_capture;
    size_t last_psram_after_release;
    esp_err_t last_error;
    const char *last_error_phase;
    bool has_last_error;
    bool memory_ok;
} nb_camera_diag_status_t;

typedef enum {
    NB_CAMERA_EVT_SESSION_ACTIVE = 0,
    NB_CAMERA_EVT_SESSION_INACTIVE,
} nb_camera_event_t;

typedef void (*nb_camera_event_cb_t)(nb_camera_event_t evt);

esp_err_t camera_service_init(void);
void camera_service_set_event_cb(nb_camera_event_cb_t cb);
bool camera_service_is_supported(void);
bool camera_service_is_available(void);
bool camera_service_is_ready(void);
esp_err_t camera_service_set_mode(nb_camera_mode_t mode);
nb_camera_mode_t camera_service_get_mode(void);
const char *camera_service_format_name(int format);
void camera_service_get_diag_status(nb_camera_diag_status_t *out);
void camera_service_get_scene_metrics(nb_camera_scene_metrics_t *out);

/**
 * @brief Captura e analisa um frame sem gerar JPEG.
 *
 * Usado por visão/presença invisível para evitar custo de encode e payload.
 */
esp_err_t camera_service_observe_scene(nb_camera_observation_t *out);

/**
 * @brief Captura snapshot JPEG e empresta o buffer ao chamador.
 *
 * @param quality Define o tradeoff de qualidade JPEG. Atualmente apenas
 *                NB_CAMERA_QUALITY_SNAPSHOT e usado; a camera fica reservada
 *                para capturas curtas de visao.
 *
 * O chamador deve sempre finalizar com camera_service_release_snapshot().
 */
esp_err_t camera_service_capture_snapshot(nb_camera_snapshot_t *out,
                                          nb_camera_quality_t quality);

void camera_service_release_snapshot(void);

/**
 * @brief Fecha a sessao de camera mantida em modo quente.
 *
 * Usado quando o dashboard sai da tela de diagnostico ou quando outro servico
 * precisa liberar memoria DMA de forma deterministica.
 */
esp_err_t camera_service_close_session(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_CAMERA_SERVICE_H */
