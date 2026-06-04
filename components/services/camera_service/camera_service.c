/*
 * camera_service.c - Servico de camera do NoiseBot (Layer 4).
 */

#include "camera_service.h"

#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "linux/videodev2.h"
#include <string.h>

#define TAG "nb_camera_svc"
#define CAMERA_SVC_SESSION_HOLD_US (45ULL * 1000ULL * 1000ULL)
#define CAMERA_SVC_RETRY_CLOSE_US  (1ULL * 1000ULL * 1000ULL)
#define CAMERA_SVC_SAFE_JPEG_QUALITY   20
#define CAMERA_SVC_BETTER_JPEG_QUALITY 82
#define CAMERA_SVC_MOTION_GRID_W   16U
#define CAMERA_SVC_MOTION_GRID_H   12U
#define CAMERA_SVC_MOTION_CELLS    (CAMERA_SVC_MOTION_GRID_W * CAMERA_SVC_MOTION_GRID_H)

static bool s_initialized = false;
static SemaphoreHandle_t s_mutex = NULL;
static esp_timer_handle_t s_hold_timer = NULL;
static bool s_snapshot_borrowed = false;
static uint8_t *s_snapshot_owned_data = NULL;
static bool s_snapshot_owned_jpeg = false;
static esp_err_t s_last_error = ESP_OK;
static const char *s_last_error_phase = "";
static bool s_has_last_error = false;
static size_t s_last_frame_bytes = 0;
static size_t s_last_frame_width = 0;
static size_t s_last_frame_height = 0;
static int s_last_frame_format = 0;
static size_t s_last_jpeg_bytes = 0;
static uint32_t s_last_capture_ms = 0;
static uint32_t s_capture_count = 0;
static uint32_t s_fail_count = 0;
static size_t s_last_dma_before = 0;
static size_t s_last_dma_after_capture = 0;
static size_t s_last_dma_after_release = 0;
static size_t s_last_dma_largest_before = 0;
static size_t s_last_dma_largest_after_capture = 0;
static size_t s_last_dma_largest_after_release = 0;
static size_t s_last_internal_before = 0;
static size_t s_last_internal_after_capture = 0;
static size_t s_last_internal_after_release = 0;
static size_t s_last_psram_before = 0;
static size_t s_last_psram_after_capture = 0;
static size_t s_last_psram_after_release = 0;
static nb_camera_scene_metrics_t s_scene_metrics = {0};
static uint8_t s_motion_prev[CAMERA_SVC_MOTION_CELLS];
static bool s_motion_prev_valid = false;
static bool s_session_active = false;
static nb_camera_event_cb_t s_event_cb = NULL;

static void camera_service_set_session_active(bool active)
{
    if (s_session_active == active) {
        return;
    }
    s_session_active = active;
    nb_camera_event_cb_t cb = s_event_cb;
    if (cb) {
        cb(active ? NB_CAMERA_EVT_SESSION_ACTIVE : NB_CAMERA_EVT_SESSION_INACTIVE);
    }
}

static void camera_service_record_after_release(void)
{
    s_last_dma_after_release = heap_caps_get_free_size(MALLOC_CAP_DMA);
    s_last_dma_largest_after_release = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    s_last_internal_after_release = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_last_psram_after_release = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

static void camera_service_hold_cancel(void)
{
    if (s_hold_timer) {
        (void)esp_timer_stop(s_hold_timer);
    }
}

static void camera_service_hold_arm(uint64_t delay_us)
{
    if (s_hold_timer) {
        (void)esp_timer_stop(s_hold_timer);
        (void)esp_timer_start_once(s_hold_timer, delay_us);
    }
}

static void camera_service_hold_timer_cb(void *arg)
{
    (void)arg;
    if (!s_mutex) {
        return;
    }

    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        camera_service_hold_arm(CAMERA_SVC_RETRY_CLOSE_US);
        return;
    }

    if (!s_snapshot_borrowed && camera_hal_is_ready()) {
        camera_hal_deinit();
        camera_service_set_session_active(false);
        camera_service_record_after_release();
        ESP_LOGI(TAG, "sessao de camera encerrada por timeout");
    }

    xSemaphoreGive(s_mutex);
}

static void camera_service_reset_stats(void)
{
    s_last_error = ESP_OK;
    s_last_error_phase = "";
    s_has_last_error = false;
    s_last_frame_bytes = 0;
    s_last_frame_width = 0;
    s_last_frame_height = 0;
    s_last_frame_format = 0;
    s_last_jpeg_bytes = 0;
    s_last_capture_ms = 0;
    s_capture_count = 0;
    s_fail_count = 0;
    s_last_dma_before = 0;
    s_last_dma_after_capture = 0;
    s_last_dma_after_release = 0;
    s_last_dma_largest_before = 0;
    s_last_dma_largest_after_capture = 0;
    s_last_dma_largest_after_release = 0;
    s_last_internal_before = 0;
    s_last_internal_after_capture = 0;
    s_last_internal_after_release = 0;
    s_last_psram_before = 0;
    s_last_psram_after_capture = 0;
    s_last_psram_after_release = 0;
}

static esp_err_t camera_service_fail(esp_err_t err, const char *phase)
{
    s_last_error = err;
    s_last_error_phase = phase ? phase : "";
    s_has_last_error = err != ESP_OK;
    if (err != ESP_OK) {
        s_fail_count++;
    }
    return err;
}

static void camera_service_analyze_yuv422(const nb_camera_frame_t *frame)
{
    if (!frame || !frame->buf || frame->width == 0U || frame->height == 0U ||
        frame->len < frame->width * frame->height) {
        s_scene_metrics.valid = false;
        return;
    }

    const size_t pixels = frame->width * frame->height;
    const uint8_t *y_plane = frame->buf;
    uint32_t sum = 0;
    uint8_t min_y = 255U;
    uint8_t max_y = 0U;

    for (size_t i = 0; i < pixels; i++) {
        uint8_t y = y_plane[i];
        sum += y;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }

    uint8_t grid[CAMERA_SVC_MOTION_CELLS] = {0};
    for (uint32_t gy = 0; gy < CAMERA_SVC_MOTION_GRID_H; gy++) {
        uint32_t y0 = (uint32_t)((uint64_t)gy * frame->height / CAMERA_SVC_MOTION_GRID_H);
        uint32_t y1 = (uint32_t)((uint64_t)(gy + 1U) * frame->height / CAMERA_SVC_MOTION_GRID_H);
        if (y1 <= y0) y1 = y0 + 1U;
        if (y1 > frame->height) y1 = (uint32_t)frame->height;
        for (uint32_t gx = 0; gx < CAMERA_SVC_MOTION_GRID_W; gx++) {
            uint32_t x0 = (uint32_t)((uint64_t)gx * frame->width / CAMERA_SVC_MOTION_GRID_W);
            uint32_t x1 = (uint32_t)((uint64_t)(gx + 1U) * frame->width / CAMERA_SVC_MOTION_GRID_W);
            if (x1 <= x0) x1 = x0 + 1U;
            if (x1 > frame->width) x1 = (uint32_t)frame->width;

            uint32_t cell_sum = 0;
            uint32_t cell_count = 0;
            for (uint32_t yy = y0; yy < y1; yy++) {
                const uint8_t *row = y_plane + ((size_t)yy * frame->width);
                for (uint32_t xx = x0; xx < x1; xx++) {
                    cell_sum += row[xx];
                    cell_count++;
                }
            }
            uint32_t idx = gy * CAMERA_SVC_MOTION_GRID_W + gx;
            grid[idx] = (uint8_t)(cell_count > 0U ? (cell_sum / cell_count) : 0U);
        }
    }

    uint32_t motion_sum = 0;
    if (s_motion_prev_valid) {
        for (uint32_t i = 0; i < CAMERA_SVC_MOTION_CELLS; i++) {
            int diff = (int)grid[i] - (int)s_motion_prev[i];
            motion_sum += (uint32_t)(diff < 0 ? -diff : diff);
        }
    }
    memcpy(s_motion_prev, grid, sizeof(s_motion_prev));
    s_motion_prev_valid = true;

    s_scene_metrics.valid = true;
    s_scene_metrics.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    s_scene_metrics.width = frame->width;
    s_scene_metrics.height = frame->height;
    s_scene_metrics.luma_avg = (uint8_t)(sum / pixels);
    s_scene_metrics.luma_min = min_y;
    s_scene_metrics.luma_max = max_y;
    s_scene_metrics.contrast = (uint8_t)(max_y - min_y);
    s_scene_metrics.motion_score = s_motion_prev_valid
                                 ? (uint8_t)(motion_sum / CAMERA_SVC_MOTION_CELLS)
                                 : 0U;
}

static void camera_service_mark_scene_unknown(const nb_camera_frame_t *frame)
{
    s_scene_metrics.valid = false;
    s_scene_metrics.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    s_scene_metrics.width = frame ? frame->width : 0U;
    s_scene_metrics.height = frame ? frame->height : 0U;
    s_scene_metrics.luma_avg = 0U;
    s_scene_metrics.luma_min = 0U;
    s_scene_metrics.luma_max = 0U;
    s_scene_metrics.contrast = 0U;
    s_scene_metrics.motion_score = 0U;
}

static esp_err_t camera_service_encode_yuv422_to_jpeg(const nb_camera_frame_t *frame,
                                                       nb_camera_snapshot_t *out)
{
    if (!frame || !frame->buf || !out ||
        frame->width == 0U || frame->height == 0U ||
        frame->len < frame->width * frame->height * 2U) {
        return ESP_ERR_INVALID_ARG;
    }

    camera_service_analyze_yuv422(frame);
    int quality = (camera_hal_get_mode() == NB_CAMERA_MODE_SAFE_QQVGA)
                ? CAMERA_SVC_SAFE_JPEG_QUALITY
                : CAMERA_SVC_BETTER_JPEG_QUALITY;

    jpeg_enc_config_t enc_config = {
        .width = (int)frame->width,
        .height = (int)frame->height,
        .src_type = JPEG_PIXEL_FORMAT_YCbYCr,
        .subsampling = JPEG_SUBSAMPLE_422,
        .quality = quality,
    };

    jpeg_enc_handle_t enc = NULL;
    esp_err_t err = jpeg_enc_open(&enc_config, &enc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_enc_open falhou: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t jpeg_capacity = (uint32_t)(frame->width * frame->height * 3U / 4U);
    if (jpeg_capacity < 4096U) {
        jpeg_capacity = 4096U;
    }
    uint8_t *jpeg_buf = jpeg_calloc_align(jpeg_capacity, 128);
    if (!jpeg_buf) {
        jpeg_enc_close(enc);
        ESP_LOGE(TAG, "falha ao alocar buffer JPEG %lu bytes", (unsigned long)jpeg_capacity);
        return ESP_ERR_NO_MEM;
    }

    int jpeg_len = 0;
    err = jpeg_enc_process(enc,
                           frame->buf,
                           (int)frame->len,
                           jpeg_buf,
                           (int)jpeg_capacity,
                           &jpeg_len);
    jpeg_enc_close(enc);
    if (err != ESP_OK || jpeg_len <= 0) {
        jpeg_free_align(jpeg_buf);
        ESP_LOGE(TAG, "jpeg_enc_process falhou: %s len=%d", esp_err_to_name(err), jpeg_len);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    out->data = jpeg_buf;
    out->len = (size_t)jpeg_len;
    out->width = frame->width;
    out->height = frame->height;
    out->format = (int)V4L2_PIX_FMT_JPEG;
    s_snapshot_owned_data = jpeg_buf;
    s_snapshot_owned_jpeg = true;
    return ESP_OK;
}

esp_err_t camera_service_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "camera_service ja inicializado");
        return ESP_ERR_INVALID_STATE;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "falha ao criar mutex");
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = camera_service_hold_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "nb_cam_hold",
        .skip_unhandled_events = true,
    };
    esp_err_t timer_err = esp_timer_create(&timer_args, &s_hold_timer);
    if (timer_err != ESP_OK) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        ESP_LOGE(TAG, "falha ao criar timer de sessao: %s", esp_err_to_name(timer_err));
        return timer_err;
    }

    s_initialized = true;
    if (camera_hal_is_supported()) {
        ESP_LOGI(TAG, "camera_service inicializado em modo sessao");
    } else {
        ESP_LOGW(TAG, "camera_service stub: driver real fora deste build");
    }
    return ESP_OK;
}

bool camera_service_is_ready(void)
{
    return s_initialized && camera_hal_is_ready();
}

void camera_service_set_event_cb(nb_camera_event_cb_t cb)
{
    s_event_cb = cb;
}

bool camera_service_is_supported(void)
{
    return camera_hal_is_supported();
}

bool camera_service_is_available(void)
{
    return s_initialized && camera_hal_is_supported();
}

esp_err_t camera_service_set_mode(nb_camera_mode_t mode)
{
    if (!s_initialized) {
        esp_err_t svc_err = camera_service_init();
        if (svc_err != ESP_OK && svc_err != ESP_ERR_INVALID_STATE) {
            return camera_service_fail(svc_err, "service_init");
        }
    }
    if (!s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_snapshot_borrowed) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    camera_service_hold_cancel();
    if (camera_hal_is_ready()) {
        camera_hal_deinit();
        camera_service_set_session_active(false);
        camera_service_record_after_release();
    }

    esp_err_t err = camera_hal_set_mode(mode);
    if (err == ESP_OK) {
        camera_service_reset_stats();
    }
    xSemaphoreGive(s_mutex);
    return err;
}

nb_camera_mode_t camera_service_get_mode(void)
{
    return camera_hal_get_mode();
}

const char *camera_service_format_name(int format)
{
    switch (format) {
    case V4L2_PIX_FMT_YUV422P: return "yuv422";
    case V4L2_PIX_FMT_JPEG: return "jpeg";
    case 0: return "pending";
    default: return "unknown";
    }
}

void camera_service_get_diag_status(nb_camera_diag_status_t *out)
{
    if (!out) {
        return;
    }

    out->dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    out->dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    out->internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->mode = camera_hal_get_mode();
    out->mode_name = camera_hal_mode_name(out->mode);
    out->mode_width = camera_hal_mode_width(out->mode);
    out->mode_height = camera_hal_mode_height(out->mode);
    out->last_frame_bytes = s_last_frame_bytes;
    out->last_frame_width = s_last_frame_width;
    out->last_frame_height = s_last_frame_height;
    out->last_frame_format = s_last_frame_format;
    out->min_dma_before = camera_hal_mode_min_dma_before(out->mode);
    out->min_dma_largest = camera_hal_mode_min_dma_largest(out->mode);
    out->min_internal_before = camera_hal_mode_min_internal_before(out->mode);
    out->last_jpeg_bytes = s_last_jpeg_bytes;
    out->last_capture_ms = s_last_capture_ms;
    out->capture_count = s_capture_count;
    out->fail_count = s_fail_count;
    out->last_dma_before = s_last_dma_before;
    out->last_dma_after_capture = s_last_dma_after_capture;
    out->last_dma_after_release = s_last_dma_after_release;
    out->last_dma_largest_before = s_last_dma_largest_before;
    out->last_dma_largest_after_capture = s_last_dma_largest_after_capture;
    out->last_dma_largest_after_release = s_last_dma_largest_after_release;
    out->last_internal_before = s_last_internal_before;
    out->last_internal_after_capture = s_last_internal_after_capture;
    out->last_internal_after_release = s_last_internal_after_release;
    out->last_psram_before = s_last_psram_before;
    out->last_psram_after_capture = s_last_psram_after_capture;
    out->last_psram_after_release = s_last_psram_after_release;
    out->last_error = s_last_error;
    out->last_error_phase = s_last_error_phase;
    out->has_last_error = s_has_last_error;
    out->memory_ok = out->dma_free >= out->min_dma_before &&
                     out->internal_free >= out->min_internal_before;
}

void camera_service_get_scene_metrics(nb_camera_scene_metrics_t *out)
{
    if (!out) {
        return;
    }
    *out = s_scene_metrics;
}

esp_err_t camera_service_observe_scene(nb_camera_observation_t *out)
{
    if (!out) {
        return camera_service_fail(ESP_ERR_INVALID_ARG, "arg");
    }
    memset(out, 0, sizeof(*out));
    if (!camera_hal_is_supported()) {
        return camera_service_fail(ESP_ERR_NOT_SUPPORTED, "unsupported");
    }

    if (!s_initialized) {
        esp_err_t svc_err = camera_service_init();
        if (svc_err != ESP_OK && svc_err != ESP_ERR_INVALID_STATE) {
            return camera_service_fail(svc_err, "service_init");
        }
    }
    if (!s_mutex) {
        return camera_service_fail(ESP_ERR_INVALID_STATE, "mutex");
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_snapshot_borrowed) {
        xSemaphoreGive(s_mutex);
        return camera_service_fail(ESP_ERR_INVALID_STATE, "borrowed");
    }

    camera_service_hold_cancel();
    nb_camera_diag_status_t diag;
    camera_service_get_diag_status(&diag);
    s_last_dma_before = diag.dma_free;
    s_last_dma_largest_before = diag.dma_largest;
    s_last_internal_before = diag.internal_free;
    s_last_psram_before = diag.psram_free;

    if (!camera_hal_is_ready()) {
        esp_err_t init_err = camera_hal_init();
        if (init_err != ESP_OK) {
            xSemaphoreGive(s_mutex);
            return camera_service_fail(init_err, "hal_init");
        }
        camera_service_set_session_active(true);
    }

    int64_t capture_start_us = esp_timer_get_time();
    esp_err_t err = camera_hal_capture();
    if (err != ESP_OK) {
        camera_hal_deinit();
        camera_service_set_session_active(false);
        camera_service_record_after_release();
        xSemaphoreGive(s_mutex);
        return camera_service_fail(err, "capture");
    }

    nb_camera_frame_t *frame = camera_hal_get_frame();
    if (!frame || !frame->buf || frame->len == 0U) {
        camera_hal_release_frame();
        camera_hal_deinit();
        camera_service_set_session_active(false);
        camera_service_record_after_release();
        xSemaphoreGive(s_mutex);
        return camera_service_fail(ESP_FAIL, "frame");
    }

    if (frame->format == (int)V4L2_PIX_FMT_YUV422P) {
        camera_service_analyze_yuv422(frame);
    } else {
        camera_service_mark_scene_unknown(frame);
    }

    out->valid = s_scene_metrics.valid;
    out->frame_bytes = frame->len;
    out->format = frame->format;
    out->scene = s_scene_metrics;
    s_last_frame_bytes = frame->len;
    s_last_frame_width = frame->width;
    s_last_frame_height = frame->height;
    s_last_frame_format = frame->format;
    camera_hal_release_frame();

    s_last_dma_after_capture = heap_caps_get_free_size(MALLOC_CAP_DMA);
    s_last_dma_largest_after_capture = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    s_last_internal_after_capture = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_last_psram_after_capture = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s_last_jpeg_bytes = 0U;
    int64_t capture_elapsed_us = esp_timer_get_time() - capture_start_us;
    s_last_capture_ms = (uint32_t)(capture_elapsed_us / 1000LL);
    if (capture_elapsed_us > 0 && s_last_capture_ms == 0U) {
        s_last_capture_ms = 1U;
    }
    out->capture_ms = s_last_capture_ms;
    s_capture_count++;
    s_last_error = ESP_OK;
    s_last_error_phase = "";
    s_has_last_error = false;
    camera_service_record_after_release();
    if (camera_hal_is_ready()) {
        camera_service_hold_arm(CAMERA_SVC_SESSION_HOLD_US);
    }
    ESP_LOGI(TAG, "observacao ok frame=%u capture_ms=%lu count=%lu",
             (unsigned)out->frame_bytes,
             (unsigned long)s_last_capture_ms,
             (unsigned long)s_capture_count);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t camera_service_capture_snapshot(nb_camera_snapshot_t *out)
{
    if (!out) {
        return camera_service_fail(ESP_ERR_INVALID_ARG, "arg");
    }
    if (!camera_hal_is_supported()) {
        return camera_service_fail(ESP_ERR_NOT_SUPPORTED, "unsupported");
    }

    if (!s_initialized) {
        esp_err_t svc_err = camera_service_init();
        if (svc_err != ESP_OK && svc_err != ESP_ERR_INVALID_STATE) {
            return camera_service_fail(svc_err, "service_init");
        }
    }
    if (!s_mutex) {
        return camera_service_fail(ESP_ERR_INVALID_STATE, "mutex");
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_snapshot_borrowed) {
        xSemaphoreGive(s_mutex);
        return camera_service_fail(ESP_ERR_INVALID_STATE, "borrowed");
    }

    camera_service_hold_cancel();
    nb_camera_diag_status_t diag;
    camera_service_get_diag_status(&diag);
    s_last_dma_before = diag.dma_free;
    s_last_dma_largest_before = diag.dma_largest;
    s_last_internal_before = diag.internal_free;
    s_last_psram_before = diag.psram_free;

    if (!camera_hal_is_ready()) {
        esp_err_t init_err = camera_hal_init();
        if (init_err != ESP_OK) {
            xSemaphoreGive(s_mutex);
            return camera_service_fail(init_err, "hal_init");
        }
        camera_service_set_session_active(true);
    }

    int64_t capture_start_us = esp_timer_get_time();
    esp_err_t err = camera_hal_capture();
    if (err != ESP_OK) {
        camera_hal_deinit();
        camera_service_set_session_active(false);
        camera_service_record_after_release();
        xSemaphoreGive(s_mutex);
        return camera_service_fail(err, "capture");
    }

    nb_camera_frame_t *frame = camera_hal_get_frame();
    if (!frame || !frame->buf || frame->len == 0U) {
        camera_hal_release_frame();
        camera_hal_deinit();
        camera_service_set_session_active(false);
        camera_service_record_after_release();
        xSemaphoreGive(s_mutex);
        return camera_service_fail(ESP_FAIL, "frame");
    }

    if (s_snapshot_owned_data) {
        jpeg_free_align(s_snapshot_owned_data);
        s_snapshot_owned_data = NULL;
        s_snapshot_owned_jpeg = false;
    }

    if (frame->format == (int)V4L2_PIX_FMT_JPEG) {
        out->data = frame->buf;
        out->len = frame->len;
        out->width = frame->width;
        out->height = frame->height;
        out->format = (int)frame->format;
        s_last_frame_bytes = frame->len;
        s_last_frame_width = frame->width;
        s_last_frame_height = frame->height;
        s_last_frame_format = frame->format;
    } else if (frame->format == (int)V4L2_PIX_FMT_YUV422P) {
        s_last_frame_bytes = frame->len;
        s_last_frame_width = frame->width;
        s_last_frame_height = frame->height;
        s_last_frame_format = frame->format;
        err = camera_service_encode_yuv422_to_jpeg(frame, out);
        camera_hal_release_frame();
        if (err != ESP_OK) {
            camera_hal_deinit();
            camera_service_set_session_active(false);
            camera_service_record_after_release();
            xSemaphoreGive(s_mutex);
            return camera_service_fail(err, "jpeg_encode");
        }
    } else {
        camera_hal_release_frame();
        camera_hal_deinit();
        camera_service_set_session_active(false);
        camera_service_record_after_release();
        xSemaphoreGive(s_mutex);
        return camera_service_fail(ESP_ERR_NOT_SUPPORTED, "format");
    }
    s_snapshot_borrowed = true;
    s_last_dma_after_capture = heap_caps_get_free_size(MALLOC_CAP_DMA);
    s_last_dma_largest_after_capture = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    s_last_internal_after_capture = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_last_psram_after_capture = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s_last_jpeg_bytes = out->len;
    int64_t capture_elapsed_us = esp_timer_get_time() - capture_start_us;
    s_last_capture_ms = (uint32_t)(capture_elapsed_us / 1000LL);
    if (capture_elapsed_us > 0 && s_last_capture_ms == 0U) {
        s_last_capture_ms = 1U;
    }
    s_capture_count++;
    s_last_error = ESP_OK;
    s_last_error_phase = "";
    s_has_last_error = false;
    ESP_LOGI(TAG, "snapshot ok bytes=%u capture_ms=%lu count=%lu DMA=%uKB->%uKB maior=%uKB->%uKB INT=%uKB->%uKB",
             (unsigned)s_last_jpeg_bytes,
             (unsigned long)s_last_capture_ms,
             (unsigned long)s_capture_count,
             (unsigned)(s_last_dma_before / 1024U),
             (unsigned)(s_last_dma_after_capture / 1024U),
             (unsigned)(s_last_dma_largest_before / 1024U),
             (unsigned)(s_last_dma_largest_after_capture / 1024U),
             (unsigned)(s_last_internal_before / 1024U),
             (unsigned)(s_last_internal_after_capture / 1024U));
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void camera_service_release_snapshot(void)
{
    if (!s_mutex) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_snapshot_borrowed) {
        if (s_snapshot_owned_jpeg) {
            jpeg_free_align(s_snapshot_owned_data);
            s_snapshot_owned_data = NULL;
            s_snapshot_owned_jpeg = false;
        } else {
            camera_hal_release_frame();
        }
        s_snapshot_borrowed = false;
    }
    camera_service_record_after_release();
    if (camera_hal_is_ready()) {
        camera_service_hold_arm(CAMERA_SVC_SESSION_HOLD_US);
    }
    xSemaphoreGive(s_mutex);
}

esp_err_t camera_service_close_session(void)
{
    if (!s_initialized || !s_mutex) {
        return ESP_OK;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    camera_service_hold_cancel();
    if (s_snapshot_borrowed) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (camera_hal_is_ready()) {
        camera_hal_deinit();
        camera_service_set_session_active(false);
        camera_service_record_after_release();
        ESP_LOGI(TAG, "sessao de camera encerrada por requisicao");
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}
