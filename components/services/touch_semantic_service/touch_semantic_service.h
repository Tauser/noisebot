/*
 * touch_semantic_service.h — Touch semântico do NoiseBot (Layer 5)
 *
 * Intercepta TAP e SUSTAINED do on_touch_event (boot_manager) antes da
 * publicação no bus, adicionando semântica de alto nível:
 *
 *   NB_EVT_TOUCH_DOUBLE_TAP  — 2 taps em < 400ms
 *   NB_EVT_TOUCH_TAP         — tap único (publicado com delay de 400ms)
 *   NB_EVT_TOUCH_SUSTAINED   — publicado pelo serviço (não pelo boot_manager)
 *   NB_EVT_TOUCH_WARM_PULSE  — a cada 1s durante toque 3–8s
 *   NB_EVT_TOUCH_DEEP        — toque > 8s (dispara uma vez)
 *   NB_EVT_TOUCH_CARESS      — toque > 15s (dispara uma vez)
 *
 * on_touch_event no boot_manager deve chamar touch_semantic_on_tap() e
 * touch_semantic_on_sustained() em vez de publicar esses eventos diretamente.
 */

#ifndef NB_TOUCH_SEMANTIC_SERVICE_H
#define NB_TOUCH_SEMANTIC_SERVICE_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t touch_semantic_init(void);

/** Chamar de on_touch_event (touch_task) no lugar do publish de NB_EVT_TOUCH_TAP. */
void touch_semantic_on_tap(void);

/** Chamar de on_touch_event (touch_task) no lugar do publish de NB_EVT_TOUCH_SUSTAINED. */
void touch_semantic_on_sustained(void);

/** Tick do behavior_task (100ms). */
void touch_semantic_tick(uint32_t dt_ms);

#ifdef __cplusplus
}
#endif

#endif /* NB_TOUCH_SEMANTIC_SERVICE_H */
