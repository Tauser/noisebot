/*
 * circadian_service.c — Ritmos circadianos (Layer 5)
 */

#include "circadian_service.h"

#include "event_bus.h"
#include "nb_events.h"
#include "conductor.h"

#include "esp_log.h"

#define TAG "nb_circadian"

#define DAWN_END_MS   (30u  * 60u * 1000u)   /*  30 min */
#define DAY_END_MS    (4u  * 60u * 60u * 1000u) /*   4 h  */

static bool                  s_initialized;
static uint32_t              s_session_ms;
static nb_circadian_phase_t  s_phase;
static bool                  s_stretch_played;

static nb_circadian_phase_t phase_for(uint32_t ms)
{
    if (ms < DAWN_END_MS) return NB_CIRCADIAN_DAWN;
    if (ms < DAY_END_MS)  return NB_CIRCADIAN_DAY;
    return NB_CIRCADIAN_DUSK;
}

esp_err_t circadian_service_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    s_session_ms     = 0;
    s_phase          = NB_CIRCADIAN_DAWN;
    s_stretch_played = false;
    s_initialized    = true;
    ESP_LOGI(TAG, "circadian_service inicializado — fase DAWN");
    return ESP_OK;
}

void circadian_tick(uint32_t dt_ms)
{
    if (!s_initialized) return;

    s_session_ms += dt_ms;

    nb_circadian_phase_t new_phase = phase_for(s_session_ms);
    if (new_phase == s_phase) return;

    nb_circadian_phase_t old = s_phase;
    s_phase = new_phase;

    if (old == NB_CIRCADIAN_DAWN && new_phase == NB_CIRCADIAN_DAY && !s_stretch_played) {
        s_stretch_played = true;
        conductor_play(NB_ACTION_STRETCH);
    }

    nb_event_t ev = { .type = NB_EVT_CIRCADIAN_PHASE_CHANGED,
                      .data.u32 = (uint32_t)new_phase };
    nb_event_publish(&ev);

    const char *names[] = { "DAWN", "DAY", "DUSK" };
    ESP_LOGI(TAG, "fase: %s → %s (uptime=%lus)",
             names[old], names[new_phase], (unsigned long)(s_session_ms / 1000u));
}

nb_circadian_phase_t circadian_get_phase(void)
{
    return s_phase;
}
