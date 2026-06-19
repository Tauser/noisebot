/*
 * schedule_service.c — Timer de um disparo e recorrente (Layer 5)
 *
 * Implementação com array estático de NB_SCHED_MAX_SLOTS slots. Cada slot
 * guarda: ativo, recorrente, tempo restante, intervalo original, callback e ctx.
 *
 * Concorrência:
 *   schedule_after() e schedule_repeat() são chamados de tasks variadas →
 *   usam portENTER_CRITICAL para alocar o slot atomicamente.
 *   schedule_cancel() idem.
 *   schedule_service_tick() é chamado de uma única task (behavior_task) →
 *   lê/escreve sem lock, exceto ao ler o flag `active` de slots que podem
 *   ser cancelados concorrentemente. Para isso, usa portENTER_CRITICAL apenas
 *   no momento de disparar/rearmar/desativar, mantendo o caminho crítico curto.
 *
 * Zero malloc em todo o caminho crítico.
 */

#include "schedule_service.h"
#include "logger.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_log.h"

#include <string.h>

#define TAG "nb_sched"

/* ── Slot ────────────────────────────────────────────────────────────────── */

typedef struct {
    bool             active;
    bool             repeat;
    uint32_t         remaining_ms;   /**< Tempo até o próximo disparo. */
    uint32_t         interval_ms;    /**< Intervalo original (repeat). */
    schedule_cb_t    cb;
    void            *ctx;
} nb_sched_slot_t;

/* ── Estado global ───────────────────────────────────────────────────────── */

static struct {
    bool             initialized;
    nb_sched_slot_t  slots[NB_SCHED_MAX_SLOTS];
} s_state;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* ── Helpers internos ────────────────────────────────────────────────────── */

/**
 * Aloca um slot livre e o preenche. Retorna o índice ou
 * NB_SCHED_INVALID_HANDLE se cheio. Chamado sob s_mux.
 */
static schedule_handle_t alloc_slot(uint32_t first_ms,
                                    uint32_t interval_ms,
                                    bool     repeat,
                                    schedule_cb_t cb,
                                    void    *ctx)
{
    for (uint8_t i = 0; i < NB_SCHED_MAX_SLOTS; i++) {
        if (!s_state.slots[i].active) {
            s_state.slots[i].active       = true;
            s_state.slots[i].repeat       = repeat;
            s_state.slots[i].remaining_ms = first_ms;
            s_state.slots[i].interval_ms  = interval_ms;
            s_state.slots[i].cb           = cb;
            s_state.slots[i].ctx          = ctx;
            return (schedule_handle_t)i;
        }
    }
    return NB_SCHED_INVALID_HANDLE;
}

/* ── API ─────────────────────────────────────────────────────────────────── */

esp_err_t schedule_service_init(void)
{
    if (s_state.initialized) return ESP_ERR_INVALID_STATE;

    memset(&s_state, 0, sizeof(s_state));
    s_state.initialized = true;

    NB_LOGI(TAG, "schedule_service inicializado (%u slots)", NB_SCHED_MAX_SLOTS);
    return ESP_OK;
}

void schedule_service_tick(uint32_t dt_ms)
{
    if (!s_state.initialized) return;
    if (dt_ms == 0U) return;

    for (uint8_t i = 0; i < NB_SCHED_MAX_SLOTS; i++) {
        /* Leitura rápida do flag sem lock — se inativo, pula */
        if (!s_state.slots[i].active) continue;

        nb_sched_slot_t *slot = &s_state.slots[i];

        /* Subtrai dt, clampeando em zero para evitar underflow */
        if (dt_ms >= slot->remaining_ms) {
            slot->remaining_ms = 0U;
        } else {
            slot->remaining_ms -= dt_ms;
        }

        if (slot->remaining_ms > 0U) continue;

        /* Slot venceu — captura callback antes de alterar estado */
        schedule_cb_t cb  = slot->cb;
        void         *ctx = slot->ctx;

        if (slot->repeat) {
            /* Rearma com o intervalo original */
            slot->remaining_ms = slot->interval_ms;
        } else {
            /* One-shot: desativa o slot */
            portENTER_CRITICAL(&s_mux);
            slot->active = false;
            portEXIT_CRITICAL(&s_mux);
        }

        /* Invoca o callback fora do lock */
        if (cb != NULL) {
            cb(ctx);
        }
    }
}

schedule_handle_t schedule_after(uint32_t delay_ms, schedule_cb_t cb, void *ctx)
{
    if (!s_state.initialized) return NB_SCHED_INVALID_HANDLE;
    if (cb == NULL)            return NB_SCHED_INVALID_HANDLE;

    schedule_handle_t h;
    portENTER_CRITICAL(&s_mux);
    h = alloc_slot(delay_ms, 0U, false, cb, ctx);
    portEXIT_CRITICAL(&s_mux);

    if (h == NB_SCHED_INVALID_HANDLE) {
        NB_LOGW(TAG, "schedule_after: sem slots disponíveis");
    }
    return h;
}

schedule_handle_t schedule_repeat(uint32_t interval_ms, schedule_cb_t cb, void *ctx)
{
    if (!s_state.initialized) return NB_SCHED_INVALID_HANDLE;
    if (cb == NULL)            return NB_SCHED_INVALID_HANDLE;
    if (interval_ms == 0U)     return NB_SCHED_INVALID_HANDLE;

    schedule_handle_t h;
    portENTER_CRITICAL(&s_mux);
    h = alloc_slot(interval_ms, interval_ms, true, cb, ctx);
    portEXIT_CRITICAL(&s_mux);

    if (h == NB_SCHED_INVALID_HANDLE) {
        NB_LOGW(TAG, "schedule_repeat: sem slots disponíveis");
    }
    return h;
}

void schedule_cancel(schedule_handle_t h)
{
    if (h == NB_SCHED_INVALID_HANDLE)   return;
    if (h >= NB_SCHED_MAX_SLOTS)        return;
    if (!s_state.initialized)           return;

    portENTER_CRITICAL(&s_mux);
    s_state.slots[h].active = false;
    portEXIT_CRITICAL(&s_mux);
}
