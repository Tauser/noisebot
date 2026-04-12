/*
 * event_bus.c — Implementação do event bus do NoiseBot
 */

#include "event_bus.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "logger.h"
#include "error_policy.h"

#include <string.h>

#define TAG "nb_bus"

/* ── Pool estático ───────────────────────────────────────────────────────── */

static nb_event_t        s_pool[NB_EVENT_BUS_POOL_SIZE];
static uint32_t          s_pool_used = 0;       /* bitmask de 32 slots */
static SemaphoreHandle_t s_pool_mutex = NULL;

/* ── Subscriber table ────────────────────────────────────────────────────── */

typedef struct {
    nb_event_handler_t    handler;
    void                 *ctx;
    nb_event_sub_handle_t handle;
} nb_sub_entry_t;

/* Indexada por [tipo][slot]. slot=0 se handle==NB_EVENT_SUB_INVALID (livre). */
static nb_sub_entry_t    s_subs[NB_EVT_COUNT][NB_EVENT_BUS_MAX_SUBS_PER_TYPE];
static SemaphoreHandle_t s_subs_mutex = NULL;
static nb_event_sub_handle_t s_next_handle = 1U;

/* ── Filas assíncronas ───────────────────────────────────────────────────── */

static QueueHandle_t s_normal_queue = NULL;
static QueueHandle_t s_safety_queue = NULL;

/* ── Contadores ──────────────────────────────────────────────────────────── */

static volatile uint32_t s_dropped_total  = 0;
static volatile uint32_t s_dropped_window = 0;
static int64_t           s_last_drop_log  = 0;

#define DROP_LOG_INTERVAL_US  (10LL * 1000000LL)

/* ── Inicialização ───────────────────────────────────────────────────────── */

static bool s_initialized = false;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static bool is_safety_event(nb_event_type_t type)
{
    return (type == NB_EVT_POWER_BROWNOUT_WARN ||
            type == NB_EVT_MOTION_FAULT        ||
            type == NB_EVT_MOTION_DISABLED);
}

/* Adquire slot do pool. Retorna NULL se pool cheio. */
static nb_event_t *pool_acquire(void)
{
    xSemaphoreTake(s_pool_mutex, portMAX_DELAY);
    for (unsigned i = 0; i < NB_EVENT_BUS_POOL_SIZE; i++) {
        if (!(s_pool_used & (1u << i))) {
            s_pool_used |= (1u << i);
            xSemaphoreGive(s_pool_mutex);
            return &s_pool[i];
        }
    }
    xSemaphoreGive(s_pool_mutex);
    return NULL;
}

/* Libera slot do pool. */
static void pool_free(nb_event_t *evt)
{
    int idx = (int)(evt - s_pool);
    if (idx < 0 || idx >= (int)NB_EVENT_BUS_POOL_SIZE) return;
    xSemaphoreTake(s_pool_mutex, portMAX_DELAY);
    s_pool_used &= ~(1u << (unsigned)idx);
    xSemaphoreGive(s_pool_mutex);
}

/*
 * Entrega evento a todos os subscribers registrados para evt->type.
 * Copia a tabela localmente antes de chamar handlers para:
 *   1. Não manter o mutex durante callbacks (evita deadlock).
 *   2. Tolerar unsubscribe dentro de um handler.
 */
static void deliver_event(const nb_event_t *evt)
{
    if ((unsigned)evt->type >= NB_EVT_COUNT) return;

    nb_sub_entry_t local[NB_EVENT_BUS_MAX_SUBS_PER_TYPE];
    xSemaphoreTake(s_subs_mutex, portMAX_DELAY);
    memcpy(local, s_subs[evt->type], sizeof(local));
    xSemaphoreGive(s_subs_mutex);

    for (unsigned i = 0; i < NB_EVENT_BUS_MAX_SUBS_PER_TYPE; i++) {
        if (local[i].handle != NB_EVENT_SUB_INVALID && local[i].handler) {
            local[i].handler(evt, local[i].ctx);
        }
    }
}

/* Loga eventos dropped a cada 10s se count > 0. */
static void maybe_log_dropped(void)
{
    if (s_dropped_window == 0) return;
    int64_t now = esp_timer_get_time();
    if (now - s_last_drop_log >= DROP_LOG_INTERVAL_US) {
        NB_LOGW(TAG, "Eventos descartados nos ultimos 10s: %lu (total: %lu)",
                (unsigned long)s_dropped_window,
                (unsigned long)s_dropped_total);
        s_dropped_window = 0;
        s_last_drop_log  = now;
    }
}

/* ── Dispatcher task ──────────────────────────────────────────────────────── */

static void dispatcher_task(void *arg)
{
    (void)arg;
    nb_event_t *evt = NULL;

    s_last_drop_log = esp_timer_get_time();

    while (1) {
        /* Fila de safety tem prioridade absoluta — esvaziar completamente primeiro. */
        while (xQueueReceive(s_safety_queue, &evt, 0) == pdTRUE) {
            deliver_event(evt);
            pool_free(evt);
        }

        /* Fila normal: espera até 10ms pelo próximo evento. */
        if (xQueueReceive(s_normal_queue, &evt, pdMS_TO_TICKS(10)) == pdTRUE) {
            deliver_event(evt);
            pool_free(evt);

            /* Verificar safety novamente após cada evento normal. */
            while (xQueueReceive(s_safety_queue, &evt, 0) == pdTRUE) {
                deliver_event(evt);
                pool_free(evt);
            }
        }

        maybe_log_dropped();
    }
}

/* ── API pública ─────────────────────────────────────────────────────────── */

esp_err_t nb_event_bus_init(void)
{
    if (s_initialized) {
        NB_ASSERT(false, TAG, "nb_event_bus_init chamado mais de uma vez");
        return ESP_FAIL;
    }

    memset(s_pool, 0, sizeof(s_pool));
    memset(s_subs, 0, sizeof(s_subs));
    s_pool_used     = 0;
    s_dropped_total  = 0;
    s_dropped_window = 0;

    s_pool_mutex = xSemaphoreCreateMutex();
    s_subs_mutex = xSemaphoreCreateMutex();
    NB_ASSERT_FATAL(s_pool_mutex && s_subs_mutex, TAG,
                    "falha ao criar mutexes do event bus");

    s_normal_queue = xQueueCreate(NB_EVENT_BUS_QUEUE_SIZE,  sizeof(nb_event_t *));
    s_safety_queue = xQueueCreate(NB_EVENT_BUS_SAFETY_QUEUE_SIZE, sizeof(nb_event_t *));
    NB_ASSERT_FATAL(s_normal_queue && s_safety_queue, TAG,
                    "falha ao criar filas do event bus");

    BaseType_t ret = xTaskCreate(dispatcher_task,
                                 "nb_event_task",
                                 NB_EVENT_BUS_TASK_STACK,
                                 NULL,
                                 NB_EVENT_BUS_TASK_PRIORITY,
                                 NULL);
    NB_ASSERT_FATAL(ret == pdPASS, TAG, "falha ao criar nb_event_task");

    s_initialized = true;

    NB_LOGI(TAG, "Event bus iniciado — pool=%u, fila=%u, safety=%u, subs=%u/tipo",
            NB_EVENT_BUS_POOL_SIZE,
            NB_EVENT_BUS_QUEUE_SIZE,
            NB_EVENT_BUS_SAFETY_QUEUE_SIZE,
            NB_EVENT_BUS_MAX_SUBS_PER_TYPE);

    return ESP_OK;
}

esp_err_t nb_event_subscribe(nb_event_type_t        type,
                              nb_event_handler_t     handler,
                              void                  *ctx,
                              nb_event_sub_handle_t *out_handle)
{
    if ((unsigned)type >= NB_EVT_COUNT || !handler) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_subs_mutex, portMAX_DELAY);
    for (unsigned i = 0; i < NB_EVENT_BUS_MAX_SUBS_PER_TYPE; i++) {
        if (s_subs[type][i].handle == NB_EVENT_SUB_INVALID) {
            nb_event_sub_handle_t h = s_next_handle++;
            if (s_next_handle == NB_EVENT_SUB_INVALID) s_next_handle = 1U;
            s_subs[type][i].handler = handler;
            s_subs[type][i].ctx     = ctx;
            s_subs[type][i].handle  = h;
            if (out_handle) *out_handle = h;
            xSemaphoreGive(s_subs_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_subs_mutex);

    NB_LOGW(TAG, "subscribe falhou: tipo %d sem slot livre (max %u/tipo)",
            (int)type, NB_EVENT_BUS_MAX_SUBS_PER_TYPE);
    return ESP_ERR_NO_MEM;
}

esp_err_t nb_event_unsubscribe(nb_event_sub_handle_t handle)
{
    if (handle == NB_EVENT_SUB_INVALID) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_subs_mutex, portMAX_DELAY);
    for (int t = 0; t < NB_EVT_COUNT; t++) {
        for (unsigned i = 0; i < NB_EVENT_BUS_MAX_SUBS_PER_TYPE; i++) {
            if (s_subs[t][i].handle == handle) {
                memset(&s_subs[t][i], 0, sizeof(s_subs[t][i]));
                xSemaphoreGive(s_subs_mutex);
                return ESP_OK;
            }
        }
    }
    xSemaphoreGive(s_subs_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t nb_event_publish(const nb_event_t *evt)
{
    if (!evt || (unsigned)evt->type >= NB_EVT_COUNT) return ESP_ERR_INVALID_ARG;
    deliver_event(evt);
    return ESP_OK;
}

esp_err_t nb_event_publish_async(const nb_event_t *evt)
{
    if (!evt || (unsigned)evt->type >= NB_EVT_COUNT) return ESP_ERR_INVALID_ARG;

    nb_event_t *slot = pool_acquire();
    if (!slot) {
        s_dropped_total++;
        s_dropped_window++;
        return ESP_ERR_NO_MEM;
    }

    *slot = *evt;

    QueueHandle_t q = is_safety_event(evt->type) ? s_safety_queue : s_normal_queue;
    if (xQueueSend(q, &slot, 0) != pdTRUE) {
        pool_free(slot);
        s_dropped_total++;
        s_dropped_window++;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

uint32_t nb_event_get_dropped_count(void)
{
    return s_dropped_total;
}
