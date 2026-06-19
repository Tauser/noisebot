/*
 * circadian_service.h — Ritmos circadianos do NoiseBot (Layer 5)
 *
 * Divide a sessão em 3 fases por uptime acumulado:
 *   DAWN  0–30min   — recém acordado, lento, sonolento
 *   DAY   30min–4h  — operação normal
 *   DUSK  >4h       — cansado, yawns frequentes, timeout menor
 *
 * Publica NB_EVT_CIRCADIAN_PHASE_CHANGED (data.u32 = nb_circadian_phase_t)
 * em cada transição. Na transição DAWN→DAY dispara NB_ACTION_STRETCH.
 */

#ifndef NB_CIRCADIAN_SERVICE_H
#define NB_CIRCADIAN_SERVICE_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NB_CIRCADIAN_DAWN = 0,
    NB_CIRCADIAN_DAY  = 1,
    NB_CIRCADIAN_DUSK = 2,
} nb_circadian_phase_t;

esp_err_t            circadian_service_init(void);
void                 circadian_tick(uint32_t dt_ms);
nb_circadian_phase_t circadian_get_phase(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_CIRCADIAN_SERVICE_H */
