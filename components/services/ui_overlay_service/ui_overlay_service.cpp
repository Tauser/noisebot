/*
 * ui_overlay_service.cpp - Layer visual para feedback local rapido.
 *
 * C++ obrigatorio porque desenha diretamente no LGFX_Sprite do render_service.
 */

#include "ui_overlay_service.h"

#include "render_service.h"
#include "display_hal.h"
#include "display_lgfx_config.hpp"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstdio>
#include <cstring>

#define TAG "nb_ui"

static constexpr uint8_t OVERLAY_Z_ORDER = 30;
static constexpr int TEXT_MAX_LEN = 48;

typedef enum {
    OVERLAY_NONE = 0,
    OVERLAY_VOLUME,
    OVERLAY_TEXT,
    OVERLAY_TOAST,
} overlay_kind_t;

typedef struct {
    overlay_kind_t kind;
    nb_ui_overlay_tone_t tone;
    uint8_t        percent;
    char           text[TEXT_MAX_LEN];
    int64_t        expires_us;
} overlay_state_t;

static bool              s_initialized = false;
static SemaphoreHandle_t s_mutex = NULL;
static overlay_state_t   s_state = {};
static bool              s_was_visible = false;
static int               s_prev_x = 0;
static int               s_prev_y = 0;
static int               s_prev_w = 0;
static int               s_prev_h = 0;

static void overlay_rect(overlay_kind_t kind, int *x, int *y, int *w, int *h)
{
    int dw = display_hal_width();
    int dh = display_hal_height();
    if (dw <= 0) dw = 320;
    if (dh <= 0) dh = 240;

    if (kind == OVERLAY_TOAST) {
        *w = (dw < 308) ? (dw - 24) : 296;
        *h = 52;
        *x = (dw - *w) / 2;
        *y = 12;
    } else {
        *w = (dw < 280) ? (dw - 32) : 240;
        *h = 52;
        *x = (dw - *w) / 2;
        *y = dh - *h - 16;
    }
}

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0U) return;
    dst[0] = '\0';
    if (!src) return;

    size_t i = 0;
    for (; i + 1U < dst_size && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static void draw_speaker_icon(LGFX_Sprite *spr, int x, int y, uint16_t color)
{
    spr->fillRect(x, y + 10, 7, 14, color);
    spr->fillTriangle(x + 7, y + 10, x + 19, y + 3, x + 19, y + 31, color);
    spr->drawArc(x + 20, y + 17, 10, 8, 305, 55, color);
    spr->drawArc(x + 20, y + 17, 17, 15, 305, 55, color);
}

static void draw_volume_overlay(LGFX_Sprite *spr,
                                int x,
                                int y,
                                int w,
                                int h,
                                const overlay_state_t *state)
{
    const uint16_t fg = TFT_WHITE;
    const uint16_t dim = spr->color565(76, 88, 92);
    const uint16_t accent = spr->color565(92, 220, 186);

    spr->fillRoundRect(x, y, w, h, 6, TFT_BLACK);
    spr->drawRoundRect(x, y, w, h, 6, dim);

    draw_speaker_icon(spr, x + 16, y + 9, fg);

    int bar_x = x + 62;
    int bar_y = y + 28;
    int bar_w = w - 122;
    int fill_w = (bar_w * (int)state->percent) / 100;
    spr->fillRoundRect(bar_x, bar_y, bar_w, 8, 4, dim);
    if (fill_w > 0) {
        spr->fillRoundRect(bar_x, bar_y, fill_w, 8, 4, accent);
    }

    char label[8];
    std::snprintf(label, sizeof(label), "%u%%", (unsigned)state->percent);
    spr->setTextColor(fg, TFT_BLACK);
    spr->setTextSize(2);
    spr->drawString(label, x + w - 52, y + 18);
}

static void draw_text_overlay(LGFX_Sprite *spr,
                              int x,
                              int y,
                              int w,
                              int h,
                              const overlay_state_t *state)
{
    const uint16_t fg = TFT_WHITE;
    const uint16_t dim = spr->color565(76, 88, 92);

    spr->fillRoundRect(x, y, w, h, 6, TFT_BLACK);
    spr->drawRoundRect(x, y, w, h, 6, dim);

    spr->setTextColor(fg, TFT_BLACK);
    spr->setTextSize(2);
    spr->drawString(state->text, x + 16, y + 18);
}

static void toast_colors(LGFX_Sprite *spr,
                         nb_ui_overlay_tone_t tone,
                         uint16_t *bg,
                         uint16_t *border,
                         uint16_t *fg)
{
    switch (tone) {
        case NB_UI_OVERLAY_SUCCESS:
            *bg = spr->color565(13, 46, 31);
            *border = spr->color565(72, 208, 129);
            *fg = spr->color565(218, 255, 234);
            break;
        case NB_UI_OVERLAY_WARNING:
            *bg = spr->color565(50, 39, 8);
            *border = spr->color565(232, 184, 62);
            *fg = spr->color565(255, 243, 198);
            break;
        case NB_UI_OVERLAY_ERROR:
            *bg = spr->color565(54, 16, 28);
            *border = spr->color565(242, 96, 135);
            *fg = spr->color565(255, 224, 234);
            break;
        case NB_UI_OVERLAY_INFO:
        default:
            *bg = spr->color565(14, 31, 44);
            *border = spr->color565(84, 181, 242);
            *fg = spr->color565(226, 245, 255);
            break;
    }
}

static void draw_toast_overlay(LGFX_Sprite *spr,
                               int x,
                               int y,
                               int w,
                               int h,
                               const overlay_state_t *state)
{
    uint16_t bg, border, fg;
    toast_colors(spr, state->tone, &bg, &border, &fg);

    spr->fillRoundRect(x, y, w, h, 8, bg);
    spr->drawRoundRect(x, y, w, h, 8, border);
    spr->fillCircle(x + 22, y + 26, 6, border);

    spr->setTextColor(fg, bg);
    spr->setTextSize(2);
    spr->drawString(state->text, x + 40, y + 18);
}

static void render_layer_cb(nb_display_sprite_t canvas, void *ctx)
{
    (void)ctx;

    LGFX_Sprite *spr = static_cast<LGFX_Sprite *>(canvas);
    overlay_state_t state = {};
    bool visible = false;
    int64_t now_us = esp_timer_get_time();

    if (xSemaphoreTake(s_mutex, 0) == pdTRUE) {
        if (s_state.kind != OVERLAY_NONE && now_us < s_state.expires_us) {
            state = s_state;
            visible = true;
        } else if (s_state.kind != OVERLAY_NONE && now_us >= s_state.expires_us) {
            s_state.kind = OVERLAY_NONE;
        }
        xSemaphoreGive(s_mutex);
    }

    int x, y, w, h;
    overlay_rect(state.kind, &x, &y, &w, &h);

    if (!visible) {
        if (s_was_visible) {
            render_service_mark_dirty(s_prev_x, s_prev_y, s_prev_w, s_prev_h);
            s_was_visible = false;
        }
        return;
    }

    if (s_was_visible) {
        render_service_mark_dirty(s_prev_x, s_prev_y, s_prev_w, s_prev_h);
    }

    switch (state.kind) {
        case OVERLAY_VOLUME:
            draw_volume_overlay(spr, x, y, w, h, &state);
            break;
        case OVERLAY_TEXT:
            draw_text_overlay(spr, x, y, w, h, &state);
            break;
        case OVERLAY_TOAST:
            draw_toast_overlay(spr, x, y, w, h, &state);
            break;
        default:
            return;
    }

    render_service_mark_dirty(x, y, w, h);
    s_was_visible = true;
    s_prev_x = x;
    s_prev_y = y;
    s_prev_w = w;
    s_prev_h = h;
}

extern "C" esp_err_t ui_overlay_service_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "ui_overlay_service_init chamado mais de uma vez");
        return ESP_ERR_INVALID_STATE;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "falha ao criar mutex");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = render_service_register_layer(OVERLAY_Z_ORDER, render_layer_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "render_service_register_layer falhou: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "ui_overlay_service inicializado");
    return ESP_OK;
}

extern "C" void ui_overlay_show_volume(uint8_t percent, uint32_t duration_ms)
{
    if (!s_initialized || !s_mutex) return;
    if (percent > 100U) percent = 100U;
    if (duration_ms == 0U) duration_ms = 1600U;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.kind = OVERLAY_VOLUME;
    s_state.tone = NB_UI_OVERLAY_INFO;
    s_state.percent = percent;
    s_state.text[0] = '\0';
    s_state.expires_us = esp_timer_get_time() + ((int64_t)duration_ms * 1000LL);
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "volume overlay: %u%%", (unsigned)percent);
    render_service_force_full_refresh();
}

extern "C" void ui_overlay_show_text(const char *text, uint32_t duration_ms)
{
    if (!s_initialized || !s_mutex || !text || text[0] == '\0') return;
    if (duration_ms == 0U) duration_ms = 1600U;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.kind = OVERLAY_TEXT;
    s_state.tone = NB_UI_OVERLAY_INFO;
    s_state.percent = 0;
    copy_text(s_state.text, sizeof(s_state.text), text);
    s_state.expires_us = esp_timer_get_time() + ((int64_t)duration_ms * 1000LL);
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "text overlay: %s", s_state.text);
    render_service_force_full_refresh();
}

extern "C" void ui_overlay_show_toast(const char *text,
                                      nb_ui_overlay_tone_t tone,
                                      uint32_t duration_ms)
{
    if (!s_initialized || !s_mutex || !text || text[0] == '\0') return;
    if (duration_ms == 0U) duration_ms = 1600U;
    if ((int)tone < (int)NB_UI_OVERLAY_INFO || (int)tone > (int)NB_UI_OVERLAY_ERROR) {
        tone = NB_UI_OVERLAY_INFO;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.kind = OVERLAY_TOAST;
    s_state.tone = tone;
    s_state.percent = 0;
    copy_text(s_state.text, sizeof(s_state.text), text);
    s_state.expires_us = esp_timer_get_time() + ((int64_t)duration_ms * 1000LL);
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "toast overlay: %s", text);
    render_service_force_full_refresh();
}

extern "C" void ui_overlay_clear(void)
{
    if (!s_initialized || !s_mutex) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.kind = OVERLAY_NONE;
    s_state.text[0] = '\0';
    s_state.expires_us = 0;
    xSemaphoreGive(s_mutex);

    render_service_force_full_refresh();
}
