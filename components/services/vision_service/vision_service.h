/*
 * vision_service.h - Percepcao visual leve sobre a camera (Layer 4).
 *
 * Captura sob demanda e produz contexto local pequeno. Nao faz streaming e nao
 * chama LLM; isso mantem audio, bridge e render com prioridade operacional.
 */

#ifndef NB_VISION_SERVICE_H
#define NB_VISION_SERVICE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NB_VISION_SCENE_UNKNOWN = 0,
    NB_VISION_SCENE_DARK,
    NB_VISION_SCENE_DIM,
    NB_VISION_SCENE_NORMAL,
    NB_VISION_SCENE_BRIGHT,
    NB_VISION_SCENE_FLAT,
} nb_vision_scene_t;

typedef struct {
    bool valid;
    uint32_t timestamp_ms;
    size_t width;
    size_t height;
    size_t jpeg_bytes;
    uint32_t capture_ms;
    uint8_t luma_avg;
    uint8_t luma_min;
    uint8_t luma_max;
    uint8_t contrast;
    uint8_t motion_score;
    nb_vision_scene_t scene;
} nb_vision_observation_t;

typedef enum {
    NB_VISION_PRESENCE_UNKNOWN = 0,
    NB_VISION_PRESENCE_ABSENT,
    NB_VISION_PRESENCE_CANDIDATE,
    NB_VISION_PRESENCE_PRESENT,
} nb_vision_presence_state_t;

typedef struct {
    nb_vision_presence_state_t state;
    uint8_t score;
    uint32_t candidate_since_ms;
    uint32_t absent_since_ms;
    uint32_t last_transition_ms;
    uint32_t stable_samples;
    uint32_t absent_samples;
    uint32_t transition_count;
} nb_vision_presence_status_t;

esp_err_t vision_service_init(void);
bool vision_service_is_available(void);
esp_err_t vision_service_observe(nb_vision_observation_t *out);
esp_err_t vision_service_evaluate_presence(const nb_vision_observation_t *obs,
                                           nb_vision_presence_status_t *out);
void vision_service_get_last(nb_vision_observation_t *out);
void vision_service_get_presence(nb_vision_presence_status_t *out);
const char *vision_service_scene_name(nb_vision_scene_t scene);
const char *vision_service_presence_state_name(nb_vision_presence_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* NB_VISION_SERVICE_H */
