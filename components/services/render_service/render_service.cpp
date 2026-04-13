/*
 * render_service.cpp — Implementação do render_service do NoiseBot
 *
 * C++ obrigatório: usa LGFX_Sprite diretamente via display_lgfx_config.hpp.
 * API pública em extern "C" para compatibilidade com outros componentes C.
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
#define RENDER_TASK_PRIORITY      4
#define RENDER_TASK_CORE          1

#define TARGET_FPS                30
#define FRAME_PERIOD_US           (1000000 / TARGET_FPS)   /* 33 333 µs */
#define METRICS_LOG_PERIOD_S      5

/* ── Estado interno ──────────────────────────────────────────────────────── */

typedef struct {
    uint8_t              z_order;
    nb_render_layer_fn_t fn;
    void                *ctx;
} layer_entry_t;

static layer_entry_t      s_layers[NB_RENDER_MAX_LAYERS];
static int                s_layer_count   = 0;
static SemaphoreHandle_t  s_layer_mutex   = NULL;

static LGFX_Sprite       *s_buf[2]        = { NULL, NULL };
static int                s_back_idx      = 0;

static TaskHandle_t       s_task_handle   = NULL;
static volatile bool      s_running       = false;
static bool               s_initialized   = false;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/*
 * Insere entry na tabela mantendo ordenação por z_order (insertion sort).
 * Chamado com s_layer_mutex tomado.
 */
static void layers_insert_sorted(const layer_entry_t *entry)
{
    int pos = s_layer_count;
    for (int i = 0; i < s_layer_count; i++) {
        if (entry->z_order < s_layers[i].z_order) {
            pos = i;
            break;
        }
    }
    /* Desloca elementos à direita */
    for (int i = s_layer_count; i > pos; i--) {
        s_layers[i] = s_layers[i - 1];
    }
    s_layers[pos] = *entry;
    s_layer_count++;
}

/* ── Render task ─────────────────────────────────────────────────────────── */

static void render_task(void *arg)
{
    ESP_LOGI(TAG, "render_task iniciada — alvo=%dfps buf=%dx%d PSRAM_free=%uKB",
             TARGET_FPS,
             s_buf[0]->width(), s_buf[0]->height(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);

    int64_t metrics_start_us = esp_timer_get_time();
    int64_t next_frame_us    = metrics_start_us;
    int64_t frame_count      = 0;
    int64_t total_layer_us   = 0;
    int64_t total_push_us    = 0;

    while (s_running) {

        /* ── Desenho das layers ─────────────────────────────────────────── */

        LGFX_Sprite *canvas = s_buf[s_back_idx];
        canvas->fillScreen(TFT_BLACK);

        /* Copia a tabela de layers para fora do mutex antes de chamar callbacks. */
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

        /* ── Push DMA → display ─────────────────────────────────────────── */

        int64_t push_t0 = esp_timer_get_time();
        canvas->pushSprite(0, 0);
        total_push_us += esp_timer_get_time() - push_t0;

        /* Alterna o back buffer */
        s_back_idx ^= 1;
        frame_count++;

        /* ── Métricas periódicas ─────────────────────────────────────────── */

        int64_t now     = esp_timer_get_time();
        int64_t elapsed = now - metrics_start_us;
        if (elapsed >= (int64_t)METRICS_LOG_PERIOD_S * 1000000LL) {
            float fps          = (float)frame_count * 1e6f / (float)elapsed;
            float avg_layer_ms = (frame_count > 0)
                                    ? (float)total_layer_us / (float)frame_count / 1000.0f
                                    : 0.0f;
            float avg_push_ms  = (frame_count > 0)
                                    ? (float)total_push_us  / (float)frame_count / 1000.0f
                                    : 0.0f;
            ESP_LOGD(TAG,
                     "fps=%.1f  layer=%.2fms  push=%.2fms  PSRAM_free=%uKB",
                     fps, avg_layer_ms, avg_push_ms,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);

            frame_count    = 0;
            total_layer_us = 0;
            total_push_us  = 0;
            metrics_start_us = now;
        }

        /* ── Controle de frame rate ──────────────────────────────────────── */

        next_frame_us += FRAME_PERIOD_US;
        int64_t sleep_us = next_frame_us - esp_timer_get_time();

        if (sleep_us > 1000) {
            /* Dorme apenas milissegundos inteiros para evitar over-sleep */
            vTaskDelay(pdMS_TO_TICKS((uint32_t)(sleep_us / 1000)));
        } else if (sleep_us <= 0) {
            /*
             * Frame atrasado — reseta o deadline para evitar acúmulo de atraso
             * (spiral of death). Não dorme.
             * Log em LOGD para não agravar latência via UART no caminho crítico.
             */
            next_frame_us = esp_timer_get_time();
            ESP_LOGD(TAG, "frame atrasado %.2fms", (float)(-sleep_us) / 1000.0f);
        }
    }

    ESP_LOGI(TAG, "render_task encerrando");
    vTaskDelete(NULL);
}

/* ── API (extern "C") ────────────────────────────────────────────────────── */

extern "C" {

esp_err_t render_service_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "render_service_init chamado mais de uma vez");
        return ESP_ERR_INVALID_STATE;
    }

    s_layer_mutex = xSemaphoreCreateMutex();
    if (!s_layer_mutex) {
        ESP_LOGE(TAG, "falha ao criar mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Parent display: necessário para pushSprite() saber o destino */
    NB_LGFX *lgfx = static_cast<NB_LGFX *>(display_hal_get_lgfx());
    if (!lgfx) {
        ESP_LOGE(TAG, "display_hal_get_lgfx() retornou NULL — display não inicializado");
        vSemaphoreDelete(s_layer_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    /* Aloca dois sprites do tamanho do display em PSRAM */
    for (int i = 0; i < 2; i++) {
        s_buf[i] = new LGFX_Sprite(lgfx);
        if (!s_buf[i]) {
            ESP_LOGE(TAG, "falha ao alocar LGFX_Sprite[%d]", i);
            return ESP_ERR_NO_MEM;
        }
        s_buf[i]->setPsram(true);
        if (!s_buf[i]->createSprite(NB_DISP_WIDTH, NB_DISP_HEIGHT)) {
            ESP_LOGE(TAG,
                     "createSprite(%d,%d) falhou para buf[%d] — PSRAM livre: %uKB",
                     NB_DISP_WIDTH, NB_DISP_HEIGHT, i,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
            delete s_buf[i];
            s_buf[i] = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG,
             "render_service inicializado — 2× sprite %dx%d PSRAM (~%uKB usados)  livre=%uKB",
             NB_DISP_WIDTH, NB_DISP_HEIGHT,
             (unsigned)(NB_DISP_WIDTH * NB_DISP_HEIGHT * 2 * 2) / 1024,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);

    s_initialized = true;
    return ESP_OK;
}

esp_err_t render_service_start(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "render_service_start chamado sem init");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task_handle) {
        ESP_LOGW(TAG, "render_task já está rodando");
        return ESP_ERR_INVALID_STATE;
    }

    s_running   = true;
    s_back_idx  = 0;

    BaseType_t ret = xTaskCreatePinnedToCore(
        render_task,
        "nb_render_task",
        RENDER_TASK_STACK_BYTES,
        NULL,
        RENDER_TASK_PRIORITY,
        &s_task_handle,
        RENDER_TASK_CORE
    );

    if (ret != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore falhou (%d)", (int)ret);
        return ESP_FAIL;
    }

    return ESP_OK;
}

void render_service_stop(void)
{
    if (!s_task_handle) return;

    s_running = false;

    /*
     * Aguarda a task encerrar. A render_task chama vTaskDelete(NULL) ao sair
     * do loop, então basta esperar o handle ficar inválido.
     * Usa notificação indireta: poll com backoff curto (task a 30fps dorme
     * no máximo 33ms por iteração).
     */
    for (int i = 0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (eTaskGetState(s_task_handle) == eDeleted) break;
    }

    s_task_handle = NULL;
    ESP_LOGI(TAG, "render_service parado");
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

    ESP_LOGD(TAG, "layer registrada z=%u fn=%p total=%d", z_order, (void*)fn, s_layer_count);
    return ESP_OK;
}

void render_service_unregister_layer(nb_render_layer_fn_t fn)
{
    if (!fn) return;

    xSemaphoreTake(s_layer_mutex, portMAX_DELAY);

    for (int i = 0; i < s_layer_count; i++) {
        if (s_layers[i].fn == fn) {
            /* Desloca elementos à esquerda */
            for (int j = i; j < s_layer_count - 1; j++) {
                s_layers[j] = s_layers[j + 1];
            }
            s_layer_count--;
            xSemaphoreGive(s_layer_mutex);
            ESP_LOGD(TAG, "layer removida fn=%p total=%d", (void*)fn, s_layer_count);
            return;
        }
    }

    xSemaphoreGive(s_layer_mutex);
    ESP_LOGW(TAG, "render_service_unregister_layer: fn=%p não encontrada", (void*)fn);
}

} /* extern "C" */
