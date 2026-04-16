/*
 * render_service.cpp — Implementação do render_service do NoiseBot
 *
 * Pipeline producer-consumer (ambas as tasks em Core 1):
 *   render_task (prio 7): fillScreen + layers → acumula dirty rect → sinaliza push
 *   push_task   (prio 7): envia apenas a região suja via SPI DMA
 *
 * Dirty rect:
 *   - Cada buffer tem seu próprio dirty_rect_t que viaja com o índice.
 *   - Layers chamam render_service_mark_dirty() para declarar regiões tocadas.
 *   - Se nenhuma layer chamar mark_dirty: push completo (compatibilidade).
 *   - Se dirty_rect cobre >= FULL_PUSH_THRESHOLD do display: push completo
 *     (pushSprite único é mais eficiente que muitas chamadas row-by-row).
 *   - Se !valid após layers: frame pulado (nada mudou, zero SPI).
 *
 * Bus_SPI::wait_spi() é busy-wait — manter push em Core 1 para não bloquear
 * main (Core 0) e impedir reset do TWDT.
 */

#include "render_service.h"
#include "display_lgfx_config.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>

#define TAG "nb_render"

/* ── Parâmetros de task ──────────────────────────────────────────────────── */

#define RENDER_TASK_STACK_BYTES   4096
#define RENDER_TASK_PRIORITY      7
#define RENDER_TASK_CORE          1

#define PUSH_TASK_STACK_BYTES     3072
#define PUSH_TASK_PRIORITY        7
#define PUSH_TASK_CORE            1   /* Core 1: busy-wait SPI não afeta main/TWDT */

#define METRICS_LOG_PERIOD_S      5

/*
 * Limiar para decidir entre push parcial (row-by-row) e push completo.
 * Se dirty cobre >= 85% do display, pushSprite() único é mais eficiente
 * que iterar linha por linha.
 */
#define FULL_PUSH_THRESHOLD_PCT   85

/* ── Tipos internos ──────────────────────────────────────────────────────── */

typedef struct {
    uint8_t              z_order;
    nb_render_layer_fn_t fn;
    void                *ctx;
} layer_entry_t;

/*
 * Dirty rect por buffer. Viaja com o índice — render_task escreve no back,
 * push_task lê no front após a barreira de memória do semáforo.
 *
 * valid=false: nenhuma layer declarou dirty → skip push (frame estático).
 * force=true:  ignorar bounding box, push completo (mudança de cena).
 */
typedef struct {
    int  x0, y0, x1, y1;
    bool valid;
    bool force;
} dirty_rect_t;

/* ── Estado interno ──────────────────────────────────────────────────────── */

static layer_entry_t      s_layers[NB_RENDER_MAX_LAYERS];
static int                s_layer_count   = 0;
static SemaphoreHandle_t  s_layer_mutex   = NULL;

static LGFX_Sprite       *s_buf[2]        = { NULL, NULL };
static int                s_back_idx      = 0;
static int                s_front_idx     = 1;

static dirty_rect_t       s_dirty[2];          /* índice espelha s_buf         */
static volatile bool      s_force_full   = true; /* true = força full no 1º frame */

static SemaphoreHandle_t  s_swap_ok      = NULL;
static SemaphoreHandle_t  s_frame_ready  = NULL;

static TaskHandle_t       s_render_handle = NULL;
static TaskHandle_t       s_push_handle   = NULL;
static volatile bool      s_running       = false;
static bool               s_initialized   = false;

static NB_LGFX           *s_lgfx_ptr     = NULL;

/* Métricas lidas por render_task, escritas por push_task (int32 = atômico) */
static volatile int32_t   s_last_push_us100 = 0;
static volatile int32_t   s_last_dirty_w    = 0;
static volatile int32_t   s_last_dirty_h    = 0;
static volatile float     s_last_fps        = 0.0f;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void layers_insert_sorted(const layer_entry_t *entry)
{
    int pos = s_layer_count;
    for (int i = 0; i < s_layer_count; i++) {
        if (entry->z_order < s_layers[i].z_order) { pos = i; break; }
    }
    for (int i = s_layer_count; i > pos; i--) s_layers[i] = s_layers[i - 1];
    s_layers[pos] = *entry;
    s_layer_count++;
}

/*
 * Envia apenas o bounding box sujo para o display.
 *
 * Usa pushImage(x, row, w, 1, ptr) em loop dentro de startWrite/endWrite.
 * O bus permanece adquirido para toda a sequência — setWindow por linha é
 * o único overhead adicional vs pushSprite (CASET+RASET por linha ≈ 5µs cada).
 *
 * Para dirty regions > FULL_PUSH_THRESHOLD_PCT, push_task usa pushSprite
 * diretamente (mais eficiente para regiões grandes).
 */
static void push_dirty_region(LGFX_Sprite *canvas,
                               int x0, int y0, int x1, int y1)
{
    const uint16_t *src    = static_cast<const uint16_t *>(canvas->getBuffer());
    const int       stride = canvas->width();
    const int       w      = x1 - x0;

    s_lgfx_ptr->startWrite();
    for (int row = y0; row < y1; row++) {
        s_lgfx_ptr->pushImage(x0, row, w, 1, src + row * stride + x0);
    }
    s_lgfx_ptr->endWrite();
}

/* ── Push task (Core 1) ──────────────────────────────────────────────────── */

static void push_task(void *arg)
{
    ESP_LOGI(TAG, "push_task iniciada — Core %d", xPortGetCoreID());

    while (s_running) {
        if (xSemaphoreTake(s_frame_ready, pdMS_TO_TICKS(100)) != pdTRUE) continue;

        int fi = s_front_idx;
        const dirty_rect_t *dr = &s_dirty[fi];

        int64_t t0 = esp_timer_get_time();

        if (!dr->valid) {
            /* Frame estático — nada mudou, sem push */
            s_last_push_us100 = 0;
            s_last_dirty_w    = 0;
            s_last_dirty_h    = 0;
        } else {
            int dw = dr->x1 - dr->x0;
            int dh = dr->y1 - dr->y0;
            int dirty_pct = (dw * dh * 100) / (NB_DISP_WIDTH * NB_DISP_HEIGHT);

            if (dr->force || dirty_pct >= FULL_PUSH_THRESHOLD_PCT) {
                /* Push completo: um único transferência DMA contígua */
                s_buf[fi]->pushSprite(0, 0);
                s_last_dirty_w = NB_DISP_WIDTH;
                s_last_dirty_h = NB_DISP_HEIGHT;
            } else {
                /* Push parcial: apenas o bounding box sujo */
                push_dirty_region(s_buf[fi], dr->x0, dr->y0, dr->x1, dr->y1);
                s_last_dirty_w = dw;
                s_last_dirty_h = dh;
            }

            s_last_push_us100 = (int32_t)((esp_timer_get_time() - t0) / 100);
        }

        xSemaphoreGive(s_swap_ok);
    }

    ESP_LOGI(TAG, "push_task encerrando");
    vTaskDelete(NULL);
}

/* ── Render task (Core 1) ────────────────────────────────────────────────── */

static void render_task(void *arg)
{
    ESP_LOGI(TAG, "render_task iniciada — Core %d buf=%dx%d PSRAM_free=%uKB",
             xPortGetCoreID(),
             s_buf[0]->width(), s_buf[0]->height(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);

    int64_t metrics_start_us = esp_timer_get_time();
    int64_t frame_count      = 0;
    int64_t total_clear_us   = 0;
    int64_t total_layer_us   = 0;

    while (s_running) {

        int bi = s_back_idx;

        /* ── Inicializa dirty rect para este frame ───────────────────────── */

        bool force = s_force_full;
        if (force) s_force_full = false;

        s_dirty[bi] = (dirty_rect_t){
            .x0    = NB_DISP_WIDTH,
            .y0    = NB_DISP_HEIGHT,
            .x1    = 0,
            .y1    = 0,
            .valid = false,
            .force = force,
        };

        /* ── Clear + layers ─────────────────────────────────────────────── */

        LGFX_Sprite *canvas = s_buf[bi];

        int64_t clear_t0 = esp_timer_get_time();
        canvas->fillScreen(TFT_BLACK);
        total_clear_us += esp_timer_get_time() - clear_t0;

        layer_entry_t local[NB_RENDER_MAX_LAYERS];
        int local_count;
        xSemaphoreTake(s_layer_mutex, portMAX_DELAY);
        memcpy(local, s_layers, sizeof(layer_entry_t) * (size_t)s_layer_count);
        local_count = s_layer_count;
        xSemaphoreGive(s_layer_mutex);

        int64_t layer_t0 = esp_timer_get_time();
        for (int i = 0; i < local_count; i++) {
            local[i].fn(static_cast<nb_display_sprite_t>(canvas), local[i].ctx);
        }
        total_layer_us += esp_timer_get_time() - layer_t0;

        /*
         * Se nenhuma layer chamou mark_dirty: assume full push (fallback).
         * Garante que código legado sem mark_dirty continue funcionando.
         */
        if (!s_dirty[bi].valid && local_count > 0) {
            s_dirty[bi] = (dirty_rect_t){
                .x0 = 0, .y0 = 0,
                .x1 = NB_DISP_WIDTH, .y1 = NB_DISP_HEIGHT,
                .valid = true, .force = true,
            };
        }

        /* ── Aguarda push_task liberar o front buffer ────────────────────── */

        xSemaphoreTake(s_swap_ok, portMAX_DELAY);

        /* ── Troca back ↔ front e sinaliza push_task ────────────────────── */

        s_front_idx = bi;
        s_back_idx ^= 1;
        frame_count++;

        xSemaphoreGive(s_frame_ready);

        /* ── Métricas periódicas ─────────────────────────────────────────── */

        int64_t now     = esp_timer_get_time();
        int64_t elapsed = now - metrics_start_us;
        if (elapsed >= (int64_t)METRICS_LOG_PERIOD_S * 1000000LL) {
            float fps          = (float)frame_count * 1e6f / (float)elapsed;
            float avg_clear_ms = frame_count > 0
                                    ? (float)total_clear_us / (float)frame_count / 1000.0f : 0.0f;
            float avg_layer_ms = frame_count > 0
                                    ? (float)total_layer_us / (float)frame_count / 1000.0f : 0.0f;
            float push_ms      = (float)s_last_push_us100 / 10.0f;
            int   dw           = s_last_dirty_w;
            int   dh           = s_last_dirty_h;

            s_last_fps = fps;
            ESP_LOGI(TAG,
                     "fps=%.1f  clear=%.2fms  layer=%.2fms  push=%.1fms"
                     "  dirty=%dx%d  PSRAM_free=%uKB",
                     fps, avg_clear_ms, avg_layer_ms, push_ms, dw, dh,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);

            frame_count      = 0;
            total_clear_us   = 0;
            total_layer_us   = 0;
            metrics_start_us = now;
        }
    }

    ESP_LOGI(TAG, "render_task encerrando");
    vTaskDelete(NULL);
}

/* ── API pública (extern "C") ────────────────────────────────────────────── */

extern "C" {

esp_err_t render_service_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "render_service_init chamado mais de uma vez");
        return ESP_ERR_INVALID_STATE;
    }

    s_layer_mutex = xSemaphoreCreateMutex();
    if (!s_layer_mutex) return ESP_ERR_NO_MEM;

    s_lgfx_ptr = static_cast<NB_LGFX *>(display_hal_get_lgfx());
    if (!s_lgfx_ptr) {
        ESP_LOGE(TAG, "display_hal não inicializado");
        vSemaphoreDelete(s_layer_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    for (int i = 0; i < 2; i++) {
        s_buf[i] = new LGFX_Sprite(s_lgfx_ptr);
        if (!s_buf[i]) return ESP_ERR_NO_MEM;
        s_buf[i]->setPsram(true);
        if (!s_buf[i]->createSprite(NB_DISP_WIDTH, NB_DISP_HEIGHT)) {
            ESP_LOGE(TAG, "createSprite(%d,%d) buf[%d] falhou — PSRAM livre=%uKB",
                     NB_DISP_WIDTH, NB_DISP_HEIGHT, i,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
            delete s_buf[i]; s_buf[i] = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG,
             "inicializado — 2× sprite %dx%d PSRAM (~%uKB)  livre=%uKB",
             NB_DISP_WIDTH, NB_DISP_HEIGHT,
             (unsigned)(NB_DISP_WIDTH * NB_DISP_HEIGHT * 2 * 2) / 1024,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);

    s_initialized = true;
    return ESP_OK;
}

esp_err_t render_service_start(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "start sem init");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_render_handle) {
        ESP_LOGW(TAG, "já rodando");
        return ESP_ERR_INVALID_STATE;
    }

    s_swap_ok = xSemaphoreCreateBinary();
    if (!s_swap_ok) return ESP_ERR_NO_MEM;
    xSemaphoreGive(s_swap_ok); /* init=1: render pode fazer o primeiro swap */

    s_frame_ready = xSemaphoreCreateBinary();
    if (!s_frame_ready) {
        vSemaphoreDelete(s_swap_ok); s_swap_ok = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_running    = true;
    s_force_full = true;   /* primeiro frame sempre full refresh */
    s_back_idx   = 0;
    s_front_idx  = 1;

    /* dirty rects inicializados como inválidos */
    for (int i = 0; i < 2; i++) {
        s_dirty[i] = (dirty_rect_t){
            .x0=NB_DISP_WIDTH, .y0=NB_DISP_HEIGHT, .x1=0, .y1=0,
            .valid=false, .force=false
        };
    }

    BaseType_t ret;

    ret = xTaskCreatePinnedToCore(push_task, "nb_push_task",
                                  PUSH_TASK_STACK_BYTES, NULL,
                                  PUSH_TASK_PRIORITY, &s_push_handle,
                                  PUSH_TASK_CORE);
    if (ret != pdPASS) {
        s_running = false;
        vSemaphoreDelete(s_swap_ok); vSemaphoreDelete(s_frame_ready);
        s_swap_ok = s_frame_ready = NULL;
        return ESP_FAIL;
    }

    ret = xTaskCreatePinnedToCore(render_task, "nb_render_task",
                                  RENDER_TASK_STACK_BYTES, NULL,
                                  RENDER_TASK_PRIORITY, &s_render_handle,
                                  RENDER_TASK_CORE);
    if (ret != pdPASS) {
        s_running = false;
        xSemaphoreGive(s_frame_ready);
        vTaskDelay(pdMS_TO_TICKS(200));
        vSemaphoreDelete(s_swap_ok); vSemaphoreDelete(s_frame_ready);
        s_swap_ok = s_frame_ready = NULL;
        s_push_handle = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "pipeline iniciado — render+push Core%d prio%d  threshold=%d%%",
             RENDER_TASK_CORE, RENDER_TASK_PRIORITY, FULL_PUSH_THRESHOLD_PCT);
    return ESP_OK;
}

void render_service_stop(void)
{
    if (!s_render_handle && !s_push_handle) return;

    s_running = false;
    if (s_swap_ok)     xSemaphoreGive(s_swap_ok);
    if (s_frame_ready) xSemaphoreGive(s_frame_ready);

    for (int i = 0; i < 30; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
        bool rd = !s_render_handle || eTaskGetState(s_render_handle) == eDeleted;
        bool pd = !s_push_handle   || eTaskGetState(s_push_handle)   == eDeleted;
        if (rd && pd) break;
    }

    s_render_handle = NULL;
    s_push_handle   = NULL;

    if (s_swap_ok)     { vSemaphoreDelete(s_swap_ok);    s_swap_ok    = NULL; }
    if (s_frame_ready) { vSemaphoreDelete(s_frame_ready); s_frame_ready = NULL; }

    ESP_LOGI(TAG, "parado");
}

esp_err_t render_service_register_layer(uint8_t z_order,
                                        nb_render_layer_fn_t fn,
                                        void *ctx)
{
    if (!fn) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_layer_mutex, portMAX_DELAY);
    if (s_layer_count >= NB_RENDER_MAX_LAYERS) {
        xSemaphoreGive(s_layer_mutex);
        ESP_LOGE(TAG, "tabela de layers cheia (%d)", NB_RENDER_MAX_LAYERS);
        return ESP_ERR_NO_MEM;
    }
    layer_entry_t entry = { z_order, fn, ctx };
    layers_insert_sorted(&entry);
    xSemaphoreGive(s_layer_mutex);

    ESP_LOGD(TAG, "layer z=%u fn=%p total=%d", z_order, (void*)fn, s_layer_count);
    return ESP_OK;
}

void render_service_unregister_layer(nb_render_layer_fn_t fn)
{
    if (!fn) return;
    xSemaphoreTake(s_layer_mutex, portMAX_DELAY);
    for (int i = 0; i < s_layer_count; i++) {
        if (s_layers[i].fn == fn) {
            for (int j = i; j < s_layer_count - 1; j++) s_layers[j] = s_layers[j + 1];
            s_layer_count--;
            xSemaphoreGive(s_layer_mutex);
            return;
        }
    }
    xSemaphoreGive(s_layer_mutex);
}

void render_service_mark_dirty(int x, int y, int w, int h)
{
    int bi = s_back_idx;
    dirty_rect_t *dr = &s_dirty[bi];

    /* Clamp aos limites do display */
    int x1 = x + w;
    int y1 = y + h;
    if (x  < 0)             x  = 0;
    if (y  < 0)             y  = 0;
    if (x1 > NB_DISP_WIDTH)  x1 = NB_DISP_WIDTH;
    if (y1 > NB_DISP_HEIGHT) y1 = NB_DISP_HEIGHT;
    if (x >= x1 || y >= y1) return;

    /* Expande o bounding box */
    if (x  < dr->x0) dr->x0 = x;
    if (y  < dr->y0) dr->y0 = y;
    if (x1 > dr->x1) dr->x1 = x1;
    if (y1 > dr->y1) dr->y1 = y1;
    dr->valid = true;
}

void render_service_force_full_refresh(void)
{
    s_force_full = true;
}

float render_service_get_fps(void)
{
    return s_last_fps;
}

} /* extern "C" */
