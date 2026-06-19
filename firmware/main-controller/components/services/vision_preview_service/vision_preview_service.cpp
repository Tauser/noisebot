/*
 * vision_preview_service.cpp — Preview da câmera no display com overlay de face.
 *
 * Pipeline:
 *   nb_vpreview_task (Core 0, Prio 3): captura JPEG a 5 FPS, salva em PSRAM.
 *   render layer (z=90): drawJpg() → JPEG buffer → drawRect() → bbox overlay.
 *   NB_EVT_BRIDGE_FACE_BOX handler: atualiza bbox e timestamp.
 */

#include "vision_preview_service.h"

extern "C" {
#include "bridge_service.h"
#include "camera_service.h"
#include "event_bus.h"
#include "nb_events.h"
#include "render_service.h"
}

#include <LovyanGFX.hpp>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "nb_vpreview";

/* ── Estado interno ──────────────────────────────────────────────────────── */

static uint8_t     *s_jpeg_buf    = nullptr;
static size_t       s_jpeg_len    = 0;
static SemaphoreHandle_t s_mutex  = nullptr;
static TaskHandle_t s_task        = nullptr;
static volatile bool s_running    = false;
static volatile bool s_enabled    = false;

/* Face bbox recebida do servidor */
static uint16_t s_box_x, s_box_y, s_box_w, s_box_h;
static uint32_t s_box_ts_ms = 0;   /* esp_timer_get_time() / 1000 */

/* Display dimensions (resolução QVGA) */
static const int kDispW = 320;
static const int kDispH = 240;

/* Câmera configurada como 240×240 YUV422 (único modo funcional no DVP desta placa).
 * Centralizar horizontalmente no display 320×240. */
static const int kCamSrcW = 240;
static const int kCamSrcH = 240;
static const int kCamW    = 240;
static const int kCamH    = 240;
static const int kCamX    = (kDispW - kCamW) / 2;  /* = 40 */
static const int kCamY    = 0;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static inline LGFX_Sprite *canvas_to_sprite(nb_display_sprite_t h)
{
    return static_cast<LGFX_Sprite *>(h);
}

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

/* ── Event handler para NB_EVT_BRIDGE_FACE_BOX ──────────────────────────── */

static void face_box_event_handler(const nb_event_t *evt, void * /*ctx*/)
{
    const nb_bridge_face_box_t *box =
        static_cast<const nb_bridge_face_box_t *>(evt->data.ptr);
    if (!box) return;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        s_box_x    = box->x;
        s_box_y    = box->y;
        s_box_w    = box->width;
        s_box_h    = box->height;
        s_box_ts_ms = now_ms();
        xSemaphoreGive(s_mutex);
    }
}

/* ── Render layer callback ───────────────────────────────────────────────── */

static void preview_layer(nb_display_sprite_t canvas, void * /*ctx*/)
{
    if (!s_enabled) return;
    if (!s_jpeg_buf || !s_mutex) return;

    LGFX_Sprite *sp = canvas_to_sprite(canvas);
    if (!sp) return;

    /* Snapshot do estado protegido */
    uint8_t *snap_buf = nullptr;
    size_t   snap_len = 0;
    uint16_t bx = 0, by = 0, bw = 0, bh = 0;
    bool     box_valid = false;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
        snap_len = s_jpeg_len;
        if (snap_len > 0) {
            snap_buf = static_cast<uint8_t *>(
                heap_caps_malloc(snap_len, MALLOC_CAP_SPIRAM));
            if (snap_buf) {
                memcpy(snap_buf, s_jpeg_buf, snap_len);
            }
        }
        uint32_t age_ms = now_ms() - s_box_ts_ms;
        if (s_box_w > 0 && age_ms < NB_VISION_PREVIEW_BOX_TTL_MS) {
            bx = s_box_x; by = s_box_y; bw = s_box_w; bh = s_box_h;
            box_valid = true;
        }
        xSemaphoreGive(s_mutex);
    }

    if (snap_buf && snap_len > 0) {
        sp->fillRect(0, 0, kDispW, kDispH, TFT_BLACK);
        sp->drawJpg(snap_buf, snap_len, kCamX, kCamY, kCamW, kCamH);
        render_service_mark_dirty(0, 0, kDispW, kDispH);
    }
    if (snap_buf) {
        heap_caps_free(snap_buf);
    }

    if (box_valid) {
        /* bbox vem em coordenadas da câmera (640×480) → escalar para display (320×240) */
        float sx = (float)kCamW / (float)kCamSrcW;
        float sy = (float)kCamH / (float)kCamSrcH;
        int rx = kCamX + (int)((float)bx * sx);
        int ry = kCamY + (int)((float)by * sy);
        int rw = (int)((float)bw * sx);
        int rh = (int)((float)bh * sy);
        sp->drawRect(rx, ry, rw, rh, NB_VISION_PREVIEW_BOX_COLOR);
        render_service_mark_dirty(rx, ry, rw, rh);
    }
}

/* ── Capture task ────────────────────────────────────────────────────────── */

static void capture_task(void * /*arg*/)
{
    const TickType_t interval = pdMS_TO_TICKS(1000U / NB_VISION_PREVIEW_FPS);

    while (s_running) {
        TickType_t t0 = xTaskGetTickCount();

        if (s_enabled && camera_service_is_ready()) {
            nb_camera_snapshot_t snap = {};
            esp_err_t err = camera_service_capture_snapshot(&snap, NB_CAMERA_QUALITY_SNAPSHOT);
            if (err == ESP_OK && snap.data && snap.len > 0) {
                if (snap.len <= NB_VISION_PREVIEW_JPEG_MAX) {
                    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        memcpy(s_jpeg_buf, snap.data, snap.len);
                        s_jpeg_len = snap.len;
                        xSemaphoreGive(s_mutex);
                    }
                }
                camera_service_release_snapshot();
            }
        }

        TickType_t elapsed = xTaskGetTickCount() - t0;
        TickType_t sleep = (elapsed < interval) ? (interval - elapsed) : 1;
        vTaskDelay(sleep);
    }

    vTaskDelete(nullptr);
}

/* ── Pública API ─────────────────────────────────────────────────────────── */

esp_err_t vision_preview_service_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "falha ao criar mutex");
        return ESP_ERR_NO_MEM;
    }

    s_jpeg_buf = static_cast<uint8_t *>(
        heap_caps_malloc(NB_VISION_PREVIEW_JPEG_MAX, MALLOC_CAP_SPIRAM));
    if (!s_jpeg_buf) {
        ESP_LOGE(TAG, "sem PSRAM para buffer JPEG (%u bytes)",
                 NB_VISION_PREVIEW_JPEG_MAX);
        return ESP_ERR_NO_MEM;
    }
    s_jpeg_len = 0;

    esp_err_t err = render_service_register_layer(
        NB_VISION_PREVIEW_LAYER_Z, preview_layer, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "falha ao registrar layer: %s", esp_err_to_name(err));
        return err;
    }

    nb_event_subscribe(NB_EVT_BRIDGE_FACE_BOX, face_box_event_handler, nullptr, nullptr);

    ESP_LOGI(TAG, "inicializado: buf=%uKB layer_z=%u fps=%u",
             NB_VISION_PREVIEW_JPEG_MAX / 1024U,
             NB_VISION_PREVIEW_LAYER_Z,
             NB_VISION_PREVIEW_FPS);
    return ESP_OK;
}

esp_err_t vision_preview_service_start(void)
{
    if (s_running) return ESP_ERR_INVALID_STATE;
    s_running = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        capture_task,
        "nb_vpreview_task",
        NB_VISION_PREVIEW_TASK_STACK,
        nullptr,
        NB_VISION_PREVIEW_TASK_PRIO,
        &s_task,
        NB_VISION_PREVIEW_TASK_CORE);

    if (ret != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "falha ao criar task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "captura iniciada");
    return ESP_OK;
}

void vision_preview_service_stop(void)
{
    s_running = false;
    /* task se auto-deleta; aguardamos a próxima janela de scheduler */
    vTaskDelay(pdMS_TO_TICKS(300));
    s_task = nullptr;
    ESP_LOGI(TAG, "captura parada");
}

void vision_preview_service_set_enabled(bool enabled)
{
    s_enabled = enabled;
    if (enabled && !s_running) {
        /* task ainda não subiu — inicia agora (lazy start) */
        vision_preview_service_start();
    } else if (!enabled && s_mutex) {
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            s_jpeg_len = 0;
            xSemaphoreGive(s_mutex);
        }
    }
}

bool vision_preview_service_is_enabled(void)
{
    return s_enabled;
}
