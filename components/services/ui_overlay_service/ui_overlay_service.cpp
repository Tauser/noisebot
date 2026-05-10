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

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define TAG "nb_ui"

/* ── Sleep bubble geometry ───────────────────────────────────────────────── */
/*
 * Bolha ciano animada desenhada à frente dos olhos durante NB_STATE_SLEEPING.
 * Geometria idêntica à que estava no expression_service — movida aqui para
 * separar overlays visuais do motor de olhos procedurais.
 */

static constexpr float   SLEEP_BUBBLE_START_DELAY_MS = 38500.0f;
static constexpr float   SLEEP_BUBBLE_PERIOD_MS = 5200.0f;
static constexpr float   SLEEP_BUBBLE_VISIBLE_RATIO = 0.32f;
static constexpr float   BUBBLE_NB_PI_F         = 3.14159265358979323846f;

/* Mesmo espaço visual usado pelo expression_service para os olhos. */
static constexpr float BUBBLE_TIP_WOBBLE_PX = 0.4f;

/* Envelope conservador da bolha em escala máxima ao redor da ponta. */
static constexpr int BUBBLE_DIRTY_LEFT_PAD = 68;
static constexpr int BUBBLE_DIRTY_TOP_PAD  = 68;
static constexpr int BUBBLE_DIRTY_W        = 78;
static constexpr int BUBBLE_DIRTY_H        = 76;

static volatile bool  s_sleep_bubble_active   = false;
static int64_t        s_sleep_bubble_start_us = 0;
static bool           s_sleep_bubble_was_active = false;
static int16_t        s_eye_left_cx = 96;
static int16_t        s_eye_right_cx = 224;
static int16_t        s_eye_cy = 122;
static int            s_bubble_prev_x = 0;
static int            s_bubble_prev_y = 0;
static int            s_bubble_prev_w = 0;
static int            s_bubble_prev_h = 0;

static inline float bubble_smoothstep(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v * v * (3.0f - 2.0f * v);
}

static inline uint32_t bubble_blend_black(uint32_t color, float alpha)
{
    uint8_t r = (uint8_t)((float)((color >> 16) & 0xFFu) * alpha);
    uint8_t g = (uint8_t)((float)((color >> 8)  & 0xFFu) * alpha);
    uint8_t b = (uint8_t)((float)(color          & 0xFFu) * alpha);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline uint32_t bubble_blend_over_rgb565(uint16_t src, uint32_t color, float alpha)
{
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    uint8_t r = (uint8_t)((((uint32_t)(src >> 11) & 0x1Fu) * 255u) / 31u);
    uint8_t g = (uint8_t)((((uint32_t)(src >> 5)  & 0x3Fu) * 255u) / 63u);
    uint8_t b = (uint8_t)(( ((uint32_t)src         & 0x1Fu) * 255u) / 31u);
    float cr = (float)((color >> 16) & 0xFFu);
    float cg = (float)((color >> 8)  & 0xFFu);
    float cb = (float)( color        & 0xFFu);
    r = (uint8_t)((float)r + (cr - (float)r) * alpha);
    g = (uint8_t)((float)g + (cg - (float)g) * alpha);
    b = (uint8_t)((float)b + (cb - (float)b) * alpha);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline void bubble_draw_alpha_pixel(LGFX_Sprite *spr,
                                           int16_t x, int16_t y,
                                           uint32_t color, float alpha)
{
    if (alpha <= 0.0f) return;
    spr->drawPixel(x, y, bubble_blend_over_rgb565(spr->readPixel(x, y), color, alpha));
}

static inline int16_t bpt_x(float anchor_x, float px, float scale)
{
    return (int16_t)(anchor_x + px * scale + (px >= 0.0f ? 0.5f : -0.5f));
}

static inline int16_t bpt_y(float anchor_y, float py, float scale)
{
    return (int16_t)(anchor_y + py * scale + (py >= 0.0f ? 0.5f : -0.5f));
}

typedef struct { int16_t x; int16_t y; } bpx_t;

static int bubble_append_cubic(bpx_t *pts, int count, int max_count,
                                float ax, float ay, float scale,
                                float x0, float y0, float x1, float y1,
                                float x2, float y2, float x3, float y3,
                                bool include_start)
{
    if (include_start && count < max_count) {
        pts[count].x = bpt_x(ax, x0, scale);
        pts[count].y = bpt_y(ay, y0, scale);
        count++;
    }
    for (int i = 1; i <= 14 && count < max_count; ++i) {
        float t = (float)i / 14.0f;
        float u = 1.0f - t;
        float x = u*u*u*x0 + 3.0f*u*u*t*x1 + 3.0f*u*t*t*x2 + t*t*t*x3;
        float y = u*u*u*y0 + 3.0f*u*u*t*y1 + 3.0f*u*t*t*y2 + t*t*t*y3;
        pts[count].x = bpt_x(ax, x, scale);
        pts[count].y = bpt_y(ay, y, scale);
        count++;
    }
    return count;
}

static int bubble_build_points(bpx_t *pts, int max_count,
                                float ax, float ay, float scale)
{
    int n = 0;
    n = bubble_append_cubic(pts, n, max_count, ax, ay, scale,
        -74.75f,-159.98f,-118.95f,-159.98f,-154.75f,-124.18f,-154.75f,-79.98f, true);
    n = bubble_append_cubic(pts, n, max_count, ax, ay, scale,
        -154.75f,-79.98f,-154.75f,-35.78f,-118.95f,0.02f,-74.75f,0.02f, false);
    n = bubble_append_cubic(pts, n, max_count, ax, ay, scale,
        -74.75f,0.02f,-61.55f,0.02f,0.0f,0.0f,0.0f,0.0f, false);
    n = bubble_append_cubic(pts, n, max_count, ax, ay, scale,
        0.0f,0.0f,-3.52f,-11.66f,-7.03f,-23.32f,-10.55f,-34.98f, false);
    n = bubble_append_cubic(pts, n, max_count, ax, ay, scale,
        -10.55f,-34.98f,-0.55f,-47.18f,5.25f,-62.98f,5.25f,-79.98f, false);
    n = bubble_append_cubic(pts, n, max_count, ax, ay, scale,
        5.25f,-79.98f,5.25f,-124.18f,-30.55f,-159.98f,-74.75f,-159.98f, false);
    return n;
}

static void bubble_draw_cubic(LGFX_Sprite *spr,
                               float ax, float ay, float scale,
                               float x0, float y0, float x1, float y1,
                               float x2, float y2, float x3, float y3,
                               uint32_t color)
{
    int16_t px = bpt_x(ax, x0, scale);
    int16_t py = bpt_y(ay, y0, scale);
    for (int i = 1; i <= 14; ++i) {
        float t = (float)i / 14.0f;
        float u = 1.0f - t;
        float x = u*u*u*x0 + 3.0f*u*u*t*x1 + 3.0f*u*t*t*x2 + t*t*t*x3;
        float y = u*u*u*y0 + 3.0f*u*u*t*y1 + 3.0f*u*t*t*y2 + t*t*t*y3;
        int16_t nx = bpt_x(ax, x, scale);
        int16_t ny = bpt_y(ay, y, scale);
        spr->drawLine(px, py, nx, ny, color);
        px = nx; py = ny;
    }
}

static void bubble_draw_outline(LGFX_Sprite *spr,
                                 float ax, float ay, float scale, uint32_t color)
{
    bubble_draw_cubic(spr, ax, ay, scale,
        -74.75f,-159.98f,-118.95f,-159.98f,-154.75f,-124.18f,-154.75f,-79.98f, color);
    bubble_draw_cubic(spr, ax, ay, scale,
        -154.75f,-79.98f,-154.75f,-35.78f,-118.95f,0.02f,-74.75f,0.02f, color);
    bubble_draw_cubic(spr, ax, ay, scale,
        -74.75f,0.02f,-61.55f,0.02f,0.0f,0.0f,0.0f,0.0f, color);
    bubble_draw_cubic(spr, ax, ay, scale,
        0.0f,0.0f,-3.52f,-11.66f,-7.03f,-23.32f,-10.55f,-34.98f, color);
    bubble_draw_cubic(spr, ax, ay, scale,
        -10.55f,-34.98f,-0.55f,-47.18f,5.25f,-62.98f,5.25f,-79.98f, color);
    bubble_draw_cubic(spr, ax, ay, scale,
        5.25f,-79.98f,5.25f,-124.18f,-30.55f,-159.98f,-74.75f,-159.98f, color);
}

static void bubble_fill_body(LGFX_Sprite *spr,
                              float ax, float ay, float scale,
                              uint32_t color, float alpha)
{
    if (alpha <= 0.0f || scale <= 0.0f) return;

    bpx_t pts[128];
    int count = bubble_build_points(pts, 128, ax, ay, scale);
    if (count < 3) return;

    int16_t min_y = pts[0].y, max_y = pts[0].y;
    for (int i = 1; i < count; ++i) {
        if (pts[i].y < min_y) min_y = pts[i].y;
        if (pts[i].y > max_y) max_y = pts[i].y;
    }

    for (int16_t y = min_y; y <= max_y; ++y) {
        int16_t xs[12];
        int n = 0;
        for (int i = 0, j = count - 1; i < count; j = i++) {
            int16_t y0 = pts[j].y, y1 = pts[i].y;
            int16_t x0 = pts[j].x, x1 = pts[i].x;
            if (((y0 <= y) && (y1 > y)) || ((y1 <= y) && (y0 > y))) {
                float t = (float)(y - y0) / (float)(y1 - y0);
                float xf = (float)x0 + ((float)x1 - (float)x0) * t;
                if (n < 12) {
                    xs[n++] = (int16_t)(xf + (xf >= 0.0f ? 0.5f : -0.5f));
                }
            }
        }
        for (int i = 1; i < n; ++i) {
            int16_t v = xs[i]; int j = i - 1;
            while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; }
            xs[j + 1] = v;
        }
        for (int i = 0; i + 1 < n; i += 2) {
            for (int16_t x = xs[i]; x <= xs[i + 1]; ++x) {
                bubble_draw_alpha_pixel(spr, x, y, color, alpha);
            }
        }
    }
}

static void sleep_bubble_phase(int64_t now_us, float *phase, float *t)
{
    int64_t delay_us   = (int64_t)(SLEEP_BUBBLE_START_DELAY_MS * 1000.0f);
    int64_t period_us  = (int64_t)(SLEEP_BUBBLE_PERIOD_MS * 1000.0f);
    int64_t elapsed_us = now_us - s_sleep_bubble_start_us;
    if (elapsed_us < 0) elapsed_us = 0;
    if (elapsed_us < delay_us) {
        *phase = 1.0f;
        *t = 1.0f;
        return;
    }
    elapsed_us -= delay_us;

    *phase = (float)(elapsed_us % period_us) / (float)period_us;
    *t = (*phase >= SLEEP_BUBBLE_VISIBLE_RATIO) ? 1.0f
                                                : (*phase / SLEEP_BUBBLE_VISIBLE_RATIO);
}

static void sleep_bubble_anchor(int64_t now_us, float *anchor_x, float *anchor_y)
{
    float phase = 0.0f;
    float t = 0.0f;
    sleep_bubble_phase(now_us, &phase, &t);
    (void)phase;

    float wobble = sinf(t * 2.0f * BUBBLE_NB_PI_F);
    float eye_center_x = ((float)s_eye_left_cx + (float)s_eye_right_cx) * 0.5f;
    *anchor_x = eye_center_x + wobble * BUBBLE_TIP_WOBBLE_PX;
    *anchor_y = (float)s_eye_cy + sinf(t * 4.0f * BUBBLE_NB_PI_F + 0.7f) * 0.25f;
}

static void sleep_bubble_dirty_rect(float anchor_x, float anchor_y,
                                    int *x, int *y, int *w, int *h)
{
    *x = (int)(anchor_x + 0.5f) - BUBBLE_DIRTY_LEFT_PAD;
    *y = (int)(anchor_y + 0.5f) - BUBBLE_DIRTY_TOP_PAD;
    *w = BUBBLE_DIRTY_W;
    *h = BUBBLE_DIRTY_H;
}

static void draw_sleep_bubble(LGFX_Sprite *spr, int64_t now_us,
                              float anchor_x, float anchor_y)
{
    float phase = 0.0f;
    float t = 0.0f;
    sleep_bubble_phase(now_us, &phase, &t);
    const uint32_t bubble_blue = 0x247CFFu;

    if (phase >= SLEEP_BUBBLE_VISIBLE_RATIO) return;   /* pausa limpa — sem bolha */

    float bubble = (t < 0.34f)
                 ? bubble_smoothstep(t / 0.34f)
                 : 1.0f - bubble_smoothstep((t - 0.34f) / 0.66f);

    if (bubble <= 0.018f) return;

    float scale = 0.035f + bubble * 0.245f;

    bubble_fill_body(spr, anchor_x, anchor_y, scale, bubble_blue, 0.055f);

    uint32_t edge = bubble_blend_black(bubble_blue, 0.92f);
    uint32_t soft = bubble_blend_black(bubble_blue, 0.32f);

    bubble_draw_outline(spr, anchor_x, anchor_y, scale, edge);
    spr->fillCircle((int32_t)(anchor_x + 0.5f), (int32_t)(anchor_y + 0.5f), 1, edge);
    if (scale > 0.18f) {
        bubble_draw_outline(spr, anchor_x + 1.0f, anchor_y, scale, soft);
    }
}

static constexpr uint8_t OVERLAY_Z_ORDER = 30;
static constexpr int TEXT_MAX_LEN = 48;

typedef enum {
    OVERLAY_NONE = 0,
    OVERLAY_VOLUME,
    OVERLAY_TEXT,
    OVERLAY_TOAST,
    OVERLAY_CLOCK,
    OVERLAY_STATUS,
    OVERLAY_CONNECTION,
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
    } else if (kind == OVERLAY_CLOCK || kind == OVERLAY_STATUS || kind == OVERLAY_CONNECTION) {
        *w = (dw < 286) ? (dw - 28) : 258;
        *h = 68;
        *x = (dw - *w) / 2;
        *y = dh - *h - 12;
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

static bool parse_percent_text(const char *text, uint8_t *percent)
{
    if (!text || !percent) return false;
    int value = -1;
    if (std::sscanf(text, "Volume %d%%", &value) != 1 &&
        std::sscanf(text, "volume %d%%", &value) != 1) {
        return false;
    }
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    *percent = (uint8_t)value;
    return true;
}

static bool is_clock_text(const char *text)
{
    return text &&
           std::strstr(text, "Agora") == text &&
           (std::strstr(text, "hora") || std::strstr(text, "minuto"));
}

static bool is_status_text(const char *text)
{
    return text && std::strstr(text, "Status:") == text;
}

static bool is_connection_text(const char *text)
{
    return text &&
           (std::strstr(text, "Rede:") == text || std::strstr(text, "Bridge:") == text);
}

static void parse_clock_text(const char *text, int *hour, int *minute)
{
    int h = 0;
    int m = 0;
    const char *p = text;
    while (p && *p && (*p < '0' || *p > '9')) p++;
    if (p && *p) {
        h = std::atoi(p);
        while (*p >= '0' && *p <= '9') p++;
        while (*p && (*p < '0' || *p > '9')) p++;
        if (*p) {
            m = std::atoi(p);
        }
    }
    if (h < 0) h = 0;
    if (h > 23) h = 23;
    if (m < 0) m = 0;
    if (m > 59) m = 59;
    *hour = h;
    *minute = m;
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

static void draw_clock_overlay(LGFX_Sprite *spr,
                               int x,
                               int y,
                               int w,
                               int h,
                               const overlay_state_t *state)
{
    const uint16_t bg = spr->color565(8, 17, 24);
    const uint16_t fg = TFT_WHITE;
    const uint16_t dim = spr->color565(95, 116, 124);
    const uint16_t accent = spr->color565(92, 220, 186);

    int hour = 0;
    int minute = 0;
    parse_clock_text(state->text, &hour, &minute);

    char time_label[8];
    std::snprintf(time_label, sizeof(time_label), "%02d:%02d", hour, minute);

    spr->fillRoundRect(x, y, w, h, 8, bg);
    spr->drawRoundRect(x, y, w, h, 8, accent);
    spr->drawCircle(x + 30, y + 30, 15, accent);
    spr->drawLine(x + 30, y + 30, x + 30, y + 19, fg);
    spr->drawLine(x + 30, y + 30, x + 40, y + 30, fg);

    spr->setTextColor(fg, bg);
    spr->setTextSize(3);
    spr->drawString(time_label, x + 60, y + 12);
    spr->setTextColor(dim, bg);
    spr->setTextSize(1);
    spr->drawString("hora local", x + 64, y + 42);
}

static void draw_status_overlay(LGFX_Sprite *spr,
                                int x,
                                int y,
                                int w,
                                int h,
                                const overlay_state_t *state)
{
    const uint16_t bg = spr->color565(9, 18, 16);
    const uint16_t fg = TFT_WHITE;
    const uint16_t dim = spr->color565(112, 132, 126);
    const uint16_t accent = spr->color565(72, 208, 129);

    int health = -1;
    int attention = -1;
    const char *health_p = std::strstr(state->text, "saude ");
    const char *attn_p = std::strstr(state->text, "atencao ");
    if (health_p) health = std::atoi(health_p + 6);
    if (attn_p) attention = std::atoi(attn_p + 8);
    if (health < 0) health = -1;
    if (health > 100) health = 100;
    if (attention < 0) attention = -1;
    if (attention > 100) attention = 100;

    spr->fillRoundRect(x, y, w, h, 8, bg);
    spr->drawRoundRect(x, y, w, h, 8, accent);
    spr->fillCircle(x + 28, y + 28, 12, accent);
    spr->drawLine(x + 21, y + 28, x + 26, y + 34, bg);
    spr->drawLine(x + 26, y + 34, x + 37, y + 21, bg);

    spr->setTextColor(fg, bg);
    spr->setTextSize(2);
    spr->drawString("Status", x + 54, y + 10);
    spr->setTextColor(dim, bg);
    spr->setTextSize(1);
    if (health >= 0 && attention >= 0) {
        char line[32];
        std::snprintf(line, sizeof(line), "saude %d%%  atencao %d%%", health, attention);
        spr->drawString(line, x + 56, y + 39);
    } else {
        spr->drawString("operacional", x + 56, y + 39);
    }
}

static void draw_connection_overlay(LGFX_Sprite *spr,
                                    int x,
                                    int y,
                                    int w,
                                    int h,
                                    const overlay_state_t *state)
{
    const uint16_t bg = spr->color565(12, 18, 30);
    const uint16_t fg = TFT_WHITE;
    const uint16_t dim = spr->color565(118, 136, 154);
    const uint16_t accent = spr->color565(84, 181, 242);
    const bool bridge = std::strstr(state->text, "Bridge:") == state->text;
    const char *label = bridge ? "Bridge" : "Rede";
    const char *line = bridge ? "ouvindo" : "bridge conectado";

    spr->fillRoundRect(x, y, w, h, 8, bg);
    spr->drawRoundRect(x, y, w, h, 8, accent);
    spr->drawCircle(x + 30, y + 34, 16, accent);
    spr->drawArc(x + 30, y + 34, 10, 8, 210, 330, fg);
    spr->drawArc(x + 30, y + 34, 17, 15, 210, 330, accent);
    spr->fillCircle(x + 30, y + 36, 3, fg);

    spr->setTextColor(fg, bg);
    spr->setTextSize(2);
    spr->drawString(label, x + 58, y + 10);
    spr->setTextColor(dim, bg);
    spr->setTextSize(1);
    spr->drawString(line, x + 60, y + 38);
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

    /* Sleep bubble: animação contínua, independente dos overlays de UI. */
    bool bubble = s_sleep_bubble_active;
    if (bubble) {
        float anchor_x = 0.0f;
        float anchor_y = 0.0f;
        int dirty_x = 0, dirty_y = 0, dirty_w = 0, dirty_h = 0;
        sleep_bubble_anchor(now_us, &anchor_x, &anchor_y);
        sleep_bubble_dirty_rect(anchor_x, anchor_y, &dirty_x, &dirty_y, &dirty_w, &dirty_h);
        if (s_sleep_bubble_was_active) {
            render_service_mark_dirty(s_bubble_prev_x, s_bubble_prev_y,
                                      s_bubble_prev_w, s_bubble_prev_h);
        }
        draw_sleep_bubble(spr, now_us, anchor_x, anchor_y);
        render_service_mark_dirty(dirty_x, dirty_y, dirty_w, dirty_h);
        s_bubble_prev_x = dirty_x;
        s_bubble_prev_y = dirty_y;
        s_bubble_prev_w = dirty_w;
        s_bubble_prev_h = dirty_h;
    } else if (s_sleep_bubble_was_active) {
        render_service_mark_dirty(s_bubble_prev_x, s_bubble_prev_y,
                                  s_bubble_prev_w, s_bubble_prev_h);
    }
    s_sleep_bubble_was_active = bubble;

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
        case OVERLAY_CLOCK:
            draw_clock_overlay(spr, x, y, w, h, &state);
            break;
        case OVERLAY_STATUS:
            draw_status_overlay(spr, x, y, w, h, &state);
            break;
        case OVERLAY_CONNECTION:
            draw_connection_overlay(spr, x, y, w, h, &state);
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

    uint8_t percent = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (parse_percent_text(text, &percent)) {
        s_state.kind = OVERLAY_VOLUME;
        s_state.percent = percent;
    } else if (is_clock_text(text)) {
        s_state.kind = OVERLAY_CLOCK;
        s_state.percent = 0;
    } else if (is_status_text(text)) {
        s_state.kind = OVERLAY_STATUS;
        s_state.percent = 0;
    } else if (is_connection_text(text)) {
        s_state.kind = OVERLAY_CONNECTION;
        s_state.percent = 0;
    } else {
        s_state.kind = OVERLAY_TEXT;
        s_state.percent = 0;
    }
    s_state.tone = NB_UI_OVERLAY_INFO;
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

extern "C" void ui_overlay_set_eye_frame(int16_t left_cx, int16_t right_cx, int16_t eye_cy)
{
    s_eye_left_cx = left_cx;
    s_eye_right_cx = right_cx;
    s_eye_cy = eye_cy;
}

extern "C" void ui_overlay_sleep_bubble_set(bool enabled)
{
    if (enabled && !s_sleep_bubble_active) {
        s_sleep_bubble_start_us = esp_timer_get_time();
    }
    s_sleep_bubble_active = enabled;
}
