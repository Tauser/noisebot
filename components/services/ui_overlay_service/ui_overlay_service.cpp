/*
 * ui_overlay_service.cpp - Layer visual para feedback local rapido.
 *
 * C++ obrigatorio porque desenha diretamente no LGFX_Sprite do render_service.
 */

#include "ui_overlay_service.h"

#include "ui_overlay_assets.h"
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

/* ── Sleep message geometry ──────────────────────────────────────────────── */
/*
 * Balão discreto de sono durante NB_STATE_SLEEPING. Usa a mesma linguagem visual
 * das mensagens locais, em vez da antiga bolha ciano procedural.
 */

static constexpr float   SLEEP_BUBBLE_START_DELAY_MS = 1200.0f;
static constexpr float   SLEEP_BUBBLE_PERIOD_MS = 5200.0f;
static constexpr float   BUBBLE_NB_PI_F         = 3.14159265358979323846f;
static constexpr int     SLEEP_MSG_W            = 142;
static constexpr int     SLEEP_MSG_H            = 52;
static constexpr int     SLEEP_MSG_Y_OFFSET     = 24;
static constexpr int     SLEEP_MSG_MARGIN       = 6;
static constexpr int     LISTENING_ICON_W       = 24;
static constexpr int     LISTENING_ICON_H       = 24;
static constexpr int     LISTENING_ICON_MARGIN  = 8;
static constexpr int     TIMER_BADGE_W          = 94;
static constexpr int     TIMER_BADGE_H          = 20;
static constexpr int     TIMER_BADGE_MARGIN     = 8;

static volatile bool  s_sleep_bubble_active   = false;
static volatile bool  s_listening_indicator_active = false;
static volatile bool  s_timer_badge_active = false;
static volatile uint32_t s_timer_badge_remaining_ms = 0;
static int64_t        s_sleep_bubble_start_us = 0;
static bool           s_sleep_bubble_was_active = false;
static int16_t        s_eye_left_cx = 96;
static int16_t        s_eye_right_cx = 224;
static int16_t        s_eye_cy = 122;
static int            s_bubble_prev_x = 0;
static int            s_bubble_prev_y = 0;
static int            s_bubble_prev_w = 0;
static int            s_bubble_prev_h = 0;
static bool           s_listening_was_active = false;
static int            s_listening_prev_x = 0;
static int            s_listening_prev_y = 0;
static int            s_listening_prev_w = 0;
static int            s_listening_prev_h = 0;
static bool           s_timer_badge_was_active = false;
static int            s_timer_badge_prev_x = 0;
static int            s_timer_badge_prev_y = 0;
static int            s_timer_badge_prev_w = 0;
static int            s_timer_badge_prev_h = 0;

static inline float bubble_smoothstep(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v * v * (3.0f - 2.0f * v);
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
    *t = *phase;
}

static void sleep_bubble_rect(int64_t now_us, int *x, int *y, int *w, int *h)
{
    float phase = 0.0f;
    float t = 0.0f;
    sleep_bubble_phase(now_us, &phase, &t);
    float bob = sinf((phase * 2.0f * BUBBLE_NB_PI_F) + 0.35f);
    float appear = 1.0f;
    if (phase < 0.22f) {
        appear = bubble_smoothstep(phase / 0.22f);
    }
    (void)t;

    float eye_center_x = ((float)s_eye_left_cx + (float)s_eye_right_cx) * 0.5f;
    *w = SLEEP_MSG_W;
    *h = SLEEP_MSG_H;
    *x = (int)(eye_center_x + 0.5f) - (*w / 2);
    *y = (int)s_eye_cy + SLEEP_MSG_Y_OFFSET
       + (int)((bob * 3.0f * appear) + (bob >= 0.0f ? 0.5f : -0.5f));

    int dw = display_hal_width();
    int dh = display_hal_height();
    if (dw <= 0) dw = 320;
    if (dh <= 0) dh = 240;
    if (*x < SLEEP_MSG_MARGIN) *x = SLEEP_MSG_MARGIN;
    if (*x + *w > dw - SLEEP_MSG_MARGIN) *x = dw - SLEEP_MSG_MARGIN - *w;
    if (*y < SLEEP_MSG_MARGIN) *y = SLEEP_MSG_MARGIN;
    if (*y + *h > dh - SLEEP_MSG_MARGIN) *y = dh - SLEEP_MSG_MARGIN - *h;
}

static void draw_message_bubble(LGFX_Sprite *spr,
                                int x,
                                int y,
                                int w,
                                int h,
                                const char *text,
                                uint16_t bg,
                                uint16_t fg,
                                int64_t now_us);

static void draw_sleep_bubble(LGFX_Sprite *spr, int64_t now_us,
                              int x, int y, int w, int h)
{
    float phase = 0.0f;
    float t = 0.0f;
    sleep_bubble_phase(now_us, &phase, &t);
    (void)phase;
    (void)t;
    draw_message_bubble(spr, x, y, w, h, "Zzz...",
                        TFT_WHITE, spr->color565(54, 86, 108), now_us);
}

static constexpr uint8_t OVERLAY_Z_ORDER = 30;
static constexpr int TEXT_MAX_LEN = 129; /* 128 bytes + NUL, igual ao bridge */

static const lgfx::LVGLfont UI_FONT_STACKCHAN_TITLE(&MontserratSemiBold26);
static const lgfx::IFont * const UI_FONT_BODY  = &lgfx::fonts::lv_font_montserrat_16;
static const lgfx::IFont * const UI_FONT_SMALL = &lgfx::fonts::lv_font_montserrat_14;
static const lgfx::IFont * const UI_FONT_TITLE = &UI_FONT_STACKCHAN_TITLE;
static const lgfx::IFont * const UI_FONT_CLOCK = &lgfx::fonts::lv_font_montserrat_48;

static void ui_set_font(LGFX_Sprite *spr, const lgfx::IFont *font)
{
    spr->setFont(font);
    spr->setTextSize(1);
}

static void ui_reset_font(LGFX_Sprite *spr)
{
    spr->setFont(&lgfx::fonts::Font0);
    spr->setTextSize(1);
}

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
        *w = (dw < 312) ? (dw - 16) : 304;
        *h = 52;
        *x = (dw - *w) / 2;
        *y = dh - *h - 8;
    } else if (kind == OVERLAY_CLOCK) {
        *w = dw;
        *h = dh;
        *x = 0;
        *y = 0;
    } else if (kind == OVERLAY_STATUS || kind == OVERLAY_CONNECTION) {
        *w = (dw < 286) ? (dw - 28) : 258;
        *h = 68;
        *x = (dw - *w) / 2;
        *y = dh - *h - 12;
    } else if (kind == OVERLAY_TEXT) {
        *w = (dw < 312) ? (dw - 16) : 304;
        *h = 52;
        *x = (dw - *w) / 2;
        *y = dh - *h - 8;
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

static void listening_icon_rect(int *x, int *y, int *w, int *h)
{
    int dw = display_hal_width();
    if (dw <= 0) dw = 320;

    *w = LISTENING_ICON_W;
    *h = LISTENING_ICON_H;
    *x = dw - LISTENING_ICON_W - LISTENING_ICON_MARGIN;
    *y = LISTENING_ICON_MARGIN;
}

static void draw_listening_icon(LGFX_Sprite *spr, int x, int y, int w, int h, int64_t now_us)
{
    const float phase = (float)((now_us / 1000LL) % 1400LL) / 1400.0f;
    const float pulse = 0.5f + (0.5f * sinf(phase * 2.0f * BUBBLE_NB_PI_F));
    const uint8_t glow = (uint8_t)(190.0f + (pulse * 52.0f));
    const uint16_t fg = spr->color565(122, glow, 246);

    const int mx = x + (w / 2);
    const int my = y + 1;
    spr->fillRoundRect(mx - 5, my, 10, 14, 5, fg);
    spr->drawArc(mx, my + 8, 10, 9, 28, 152, fg);
    spr->drawLine(mx, my + 18, mx, my + 22, fg);
    spr->drawLine(mx - 6, my + 22, mx + 6, my + 22, fg);
}

static void timer_badge_rect(int *x, int *y, int *w, int *h)
{
    *w = TIMER_BADGE_W;
    *h = TIMER_BADGE_H;
    *x = TIMER_BADGE_MARGIN;
    *y = TIMER_BADGE_MARGIN;
}

static void format_timer_remaining(uint32_t remaining_ms, char *out, size_t out_size)
{
    if (!out || out_size == 0U) return;

    uint32_t total_s = remaining_ms / 1000U;
    if ((remaining_ms % 1000U) != 0U) {
        total_s++;
    }
    uint32_t hours = total_s / 3600U;
    uint32_t minutes = (total_s / 60U) % 60U;
    uint32_t seconds = total_s % 60U;

    if (hours > 0U) {
        if (hours > 99U) hours = 99U;
        std::snprintf(out, out_size, "%lu:%02lu",
                      (unsigned long)hours, (unsigned long)minutes);
    } else {
        std::snprintf(out, out_size, "%lu:%02lu",
                      (unsigned long)minutes, (unsigned long)seconds);
    }
}

static void draw_timer_badge(LGFX_Sprite *spr,
                             int x,
                             int y,
                             int w,
                             int h,
                             uint32_t remaining_ms)
{
    const uint16_t fg = TFT_WHITE;
    const uint16_t shadow = spr->color565(4, 9, 13);
    char label[8];
    (void)w;
    (void)h;
    format_timer_remaining(remaining_ms, label, sizeof(label));

    const int cx = x + 9;
    const int cy = y + 9;
    spr->drawCircle(cx + 1, cy + 1, 7, shadow);
    spr->drawLine(cx + 1, cy + 1, cx + 1, cy - 4, shadow);
    spr->drawLine(cx + 1, cy + 1, cx + 5, cy + 3, shadow);
    spr->drawLine(cx - 2, y + 2, cx + 4, y + 2, shadow);
    spr->drawCircle(cx, cy, 7, fg);
    spr->drawLine(cx, cy, cx, cy - 5, fg);
    spr->drawLine(cx, cy, cx + 4, cy + 2, fg);
    spr->drawLine(cx - 3, y + 1, cx + 3, y + 1, fg);

    ui_set_font(spr, UI_FONT_BODY);
    spr->setTextColor(shadow);
    spr->drawString(label, x + 24, y + 2);
    spr->setTextColor(fg);
    spr->drawString(label, x + 23, y + 1);
    ui_reset_font(spr);
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

static const char *clock_city_label(void)
{
    return "Sao Paulo";
}

static void clock_date_label(const char *text, char *out, size_t out_size)
{
    if (!out || out_size == 0U) return;
    out[0] = '\0';
    if (!text) return;

    int d = 0;
    int m = 0;
    int y = 0;
    const char *p = text;
    while (*p) {
        if (std::sscanf(p, "%2d/%2d/%4d", &d, &m, &y) == 3 &&
            d >= 1 && d <= 31 && m >= 1 && m <= 12 && y >= 2020 && y <= 2099) {
            std::snprintf(out, out_size, "%02d/%02d/%04d", d, m, y);
            return;
        }
        p++;
    }
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
    ui_set_font(spr, UI_FONT_TITLE);
    int label_w = spr->textWidth(label);
    spr->drawString(label, x + w - 18 - label_w, y + 16);
    ui_reset_font(spr);
}

typedef enum {
    PT_MARK_NONE = 0,
    PT_MARK_ACUTE,
    PT_MARK_GRAVE,
    PT_MARK_CIRC,
    PT_MARK_TILDE,
    PT_MARK_CEDILLA,
} pt_mark_t;

typedef struct {
    int x;
    int w;
    pt_mark_t mark;
} pt_text_mark_t;

static bool pt_char_from_codepoint(uint32_t cp, char *base, pt_mark_t *mark)
{
    *mark = PT_MARK_NONE;
    switch (cp) {
    case 0x00E1: *base = 'a'; *mark = PT_MARK_ACUTE; return true; /* á */
    case 0x00C1: *base = 'A'; *mark = PT_MARK_ACUTE; return true; /* Á */
    case 0x00E9: *base = 'e'; *mark = PT_MARK_ACUTE; return true; /* é */
    case 0x00C9: *base = 'E'; *mark = PT_MARK_ACUTE; return true; /* É */
    case 0x00ED: *base = 'i'; *mark = PT_MARK_ACUTE; return true; /* í */
    case 0x00CD: *base = 'I'; *mark = PT_MARK_ACUTE; return true; /* Í */
    case 0x00F3: *base = 'o'; *mark = PT_MARK_ACUTE; return true; /* ó */
    case 0x00D3: *base = 'O'; *mark = PT_MARK_ACUTE; return true; /* Ó */
    case 0x00FA: *base = 'u'; *mark = PT_MARK_ACUTE; return true; /* ú */
    case 0x00DA: *base = 'U'; *mark = PT_MARK_ACUTE; return true; /* Ú */
    case 0x00E0: *base = 'a'; *mark = PT_MARK_GRAVE; return true; /* à */
    case 0x00C0: *base = 'A'; *mark = PT_MARK_GRAVE; return true; /* À */
    case 0x00E2: *base = 'a'; *mark = PT_MARK_CIRC; return true;  /* â */
    case 0x00CA: *base = 'E'; *mark = PT_MARK_CIRC; return true;  /* Ê */
    case 0x00EA: *base = 'e'; *mark = PT_MARK_CIRC; return true;  /* ê */
    case 0x00F4: *base = 'o'; *mark = PT_MARK_CIRC; return true;  /* ô */
    case 0x00D4: *base = 'O'; *mark = PT_MARK_CIRC; return true;  /* Ô */
    case 0x00E3: *base = 'a'; *mark = PT_MARK_TILDE; return true; /* ã */
    case 0x00C3: *base = 'A'; *mark = PT_MARK_TILDE; return true; /* Ã */
    case 0x00F5: *base = 'o'; *mark = PT_MARK_TILDE; return true; /* õ */
    case 0x00D5: *base = 'O'; *mark = PT_MARK_TILDE; return true; /* Õ */
    case 0x00E7: *base = 'c'; *mark = PT_MARK_CEDILLA; return true; /* ç */
    case 0x00C7: *base = 'C'; *mark = PT_MARK_CEDILLA; return true; /* Ç */
    default:
        break;
    }
    return false;
}

static uint32_t utf8_next_codepoint(const char **p)
{
    const uint8_t *s = (const uint8_t *)(*p);
    if (s[0] < 0x80u) {
        (*p)++;
        return s[0];
    }
    if ((s[0] & 0xE0u) == 0xC0u && (s[1] & 0xC0u) == 0x80u) {
        *p += 2;
        return ((uint32_t)(s[0] & 0x1Fu) << 6) | (uint32_t)(s[1] & 0x3Fu);
    }
    if ((s[0] & 0xF0u) == 0xE0u &&
        (s[1] & 0xC0u) == 0x80u &&
        (s[2] & 0xC0u) == 0x80u) {
        *p += 3;
        return ((uint32_t)(s[0] & 0x0Fu) << 12) |
               ((uint32_t)(s[1] & 0x3Fu) << 6) |
               (uint32_t)(s[2] & 0x3Fu);
    }
    (*p)++;
    return '?';
}

static void build_pt_display_text(LGFX_Sprite *spr,
                                  const char *src,
                                  char *out,
                                  size_t out_size,
                                  pt_text_mark_t *marks,
                                  size_t *mark_count,
                                  size_t mark_capacity)
{
    if (!out || out_size == 0U) return;
    out[0] = '\0';
    *mark_count = 0;
    if (!src) return;

    size_t len = 0;
    const char *p = src;
    while (*p && len + 1U < out_size) {
        uint32_t cp = utf8_next_codepoint(&p);
        char base = '?';
        pt_mark_t mark = PT_MARK_NONE;
        if (cp < 0x80u) {
            base = (char)cp;
        } else if (!pt_char_from_codepoint(cp, &base, &mark)) {
            base = ' ';
        }

        int before_w = spr->textWidth(out);
        char tmp[2] = { base, '\0' };
        int char_w = spr->textWidth(tmp);
        out[len++] = base;
        out[len] = '\0';

        if (mark != PT_MARK_NONE && *mark_count < mark_capacity) {
            marks[*mark_count] = { before_w, char_w, mark };
            (*mark_count)++;
        }
    }
}

static void draw_pt_marks(LGFX_Sprite *spr,
                          int text_x,
                          int text_y,
                          const pt_text_mark_t *marks,
                          size_t mark_count,
                          uint16_t color)
{
    for (size_t i = 0; i < mark_count; i++) {
        int cx = text_x + marks[i].x + (marks[i].w / 2);
        switch (marks[i].mark) {
        case PT_MARK_ACUTE:
            spr->drawLine(cx - 3, text_y - 2, cx + 2, text_y - 7, color);
            break;
        case PT_MARK_GRAVE:
            spr->drawLine(cx - 2, text_y - 7, cx + 3, text_y - 2, color);
            break;
        case PT_MARK_CIRC:
            spr->drawLine(cx - 4, text_y - 2, cx, text_y - 7, color);
            spr->drawLine(cx, text_y - 7, cx + 4, text_y - 2, color);
            break;
        case PT_MARK_TILDE:
            spr->drawLine(cx - 5, text_y - 4, cx - 2, text_y - 7, color);
            spr->drawLine(cx - 2, text_y - 7, cx + 2, text_y - 4, color);
            spr->drawLine(cx + 2, text_y - 4, cx + 5, text_y - 7, color);
            break;
        case PT_MARK_CEDILLA:
            spr->drawLine(cx, text_y + 17, cx - 2, text_y + 20, color);
            spr->drawLine(cx - 2, text_y + 20, cx + 1, text_y + 22, color);
            break;
        default:
            break;
        }
    }
}

static void draw_message_bubble(LGFX_Sprite *spr,
                                int x,
                                int y,
                                int w,
                                int h,
                                const char *text,
                                uint16_t bg,
                                uint16_t fg,
                                int64_t now_us)
{
    const int pad_x = 18;

    char display_text[TEXT_MAX_LEN];
    pt_text_mark_t marks[32];
    size_t mark_count = 0;

    ui_set_font(spr, UI_FONT_BODY);
    build_pt_display_text(spr, text, display_text, sizeof(display_text),
                          marks, &mark_count, 32U);

    int text_w = spr->textWidth(display_text);
    int text_h = spr->fontHeight();
    int max_w = w - 18;
    int desired_w = text_w + (pad_x * 2);
    if (desired_w < 128) desired_w = 128;
    int bar_w = (desired_w > max_w) ? max_w : desired_w;
    int bar_h = 38;
    int tail_h = 10;
    int bar_x = x + ((w - bar_w) / 2);
    int bar_y = y + tail_h + ((h - bar_h - tail_h) / 2);
    if (bar_y < y + tail_h) bar_y = y + tail_h;
    if (bar_y + bar_h > y + h) bar_y = y + h - bar_h;
    int radius = bar_h / 2;
    int text_area_w = bar_w - (pad_x * 2);
    int text_x = bar_x + pad_x;
    int text_y = bar_y + ((bar_h - text_h) / 2) + 1;
    int tail_x = bar_x + (bar_w / 2);
    int tail_base_y = bar_y + 5;
    int tail_tip_y = bar_y - tail_h;

    spr->fillRoundRect(bar_x, bar_y, bar_w, bar_h, radius, bg);
    spr->fillTriangle(tail_x - 10, tail_base_y,
                      tail_x + 10, tail_base_y,
                      tail_x,      tail_tip_y,
                      bg);

    spr->setTextColor(fg, bg);
    if (text_w <= text_area_w) {
        spr->drawString(display_text, text_x, text_y);
        draw_pt_marks(spr, text_x, text_y, marks, mark_count, fg);
        ui_reset_font(spr);
        return;
    }

    int overflow = text_w - text_area_w;
    int64_t phase_ms = (now_us / 1000LL) % 6000LL;
    int offset = 0;
    if (phase_ms > 850LL) {
        int64_t moving_ms = phase_ms - 850LL;
        offset = (int)((moving_ms * 48LL) / 1000LL);
        int cycle = overflow + 42;
        if (cycle > 0) offset %= cycle;
    }
    if (offset > overflow) offset = overflow;

    spr->setClipRect(text_x, bar_y, text_area_w, bar_h);
    spr->drawString(display_text, text_x - offset, text_y);
    draw_pt_marks(spr, text_x - offset, text_y, marks, mark_count, fg);
    spr->clearClipRect();
    ui_reset_font(spr);
}

static void draw_text_overlay(LGFX_Sprite *spr,
                              int x,
                              int y,
                              int w,
                              int h,
                              const overlay_state_t *state,
                              int64_t now_us)
{
    draw_message_bubble(spr, x, y, w, h, state->text,
                        TFT_WHITE, spr->color565(54, 86, 108), now_us);
}

static void draw_clock_overlay(LGFX_Sprite *spr,
                               int x,
                               int y,
                               int w,
                               int h,
                               const overlay_state_t *state)
{
    const uint16_t bg = spr->color565(4, 9, 13);
    const uint16_t fg = spr->color565(242, 248, 250);
    const uint16_t dim = spr->color565(111, 130, 136);
    const uint16_t soft = spr->color565(177, 194, 198);

    int hour = 0;
    int minute = 0;
    parse_clock_text(state->text, &hour, &minute);

    char time_label[8];
    std::snprintf(time_label, sizeof(time_label), "%02d:%02d", hour, minute);
    char date_label[16];
    clock_date_label(state->text, date_label, sizeof(date_label));

    spr->fillRect(x, y, w, h, bg);

    spr->setTextColor(fg, bg);
    ui_set_font(spr, UI_FONT_CLOCK);
    int label_w = spr->textWidth(time_label);
    spr->drawString(time_label, x + ((w - label_w) / 2), y + 64);

    spr->setTextColor(soft, bg);
    ui_set_font(spr, UI_FONT_TITLE);
    const char *city = clock_city_label();
    int city_w = spr->textWidth(city);
    spr->drawString(city, x + ((w - city_w) / 2), y + 146);
    if (date_label[0] != '\0') {
        spr->setTextColor(dim, bg);
        ui_set_font(spr, UI_FONT_BODY);
        int date_w = spr->textWidth(date_label);
        spr->drawString(date_label, x + ((w - date_w) / 2), y + 171);
    }
    ui_reset_font(spr);
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
    ui_set_font(spr, UI_FONT_TITLE);
    spr->drawString("Status", x + 54, y + 10);
    spr->setTextColor(dim, bg);
    ui_set_font(spr, UI_FONT_SMALL);
    if (health >= 0 && attention >= 0) {
        char line[32];
        std::snprintf(line, sizeof(line), "saude %d%%  atencao %d%%", health, attention);
        spr->drawString(line, x + 56, y + 39);
    } else {
        spr->drawString("operacional", x + 56, y + 39);
    }
    ui_reset_font(spr);
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
    ui_set_font(spr, UI_FONT_TITLE);
    spr->drawString(label, x + 58, y + 10);
    spr->setTextColor(dim, bg);
    ui_set_font(spr, UI_FONT_SMALL);
    spr->drawString(line, x + 60, y + 38);
    ui_reset_font(spr);
}

static void toast_colors(LGFX_Sprite *spr,
                         nb_ui_overlay_tone_t tone,
                         uint16_t *bg,
                         uint16_t *border,
                         uint16_t *fg)
{
    switch (tone) {
        case NB_UI_OVERLAY_SUCCESS:
            *bg = TFT_WHITE;
            *border = TFT_WHITE;
            *fg = spr->color565(38, 119, 82);
            break;
        case NB_UI_OVERLAY_WARNING:
            *bg = TFT_WHITE;
            *border = TFT_WHITE;
            *fg = spr->color565(143, 100, 22);
            break;
        case NB_UI_OVERLAY_ERROR:
            *bg = TFT_WHITE;
            *border = TFT_WHITE;
            *fg = spr->color565(151, 50, 76);
            break;
        case NB_UI_OVERLAY_INFO:
        default:
            *bg = TFT_WHITE;
            *border = TFT_WHITE;
            *fg = spr->color565(54, 86, 108);
            break;
    }
}

static void draw_toast_overlay(LGFX_Sprite *spr,
                               int x,
                               int y,
                               int w,
                               int h,
                               const overlay_state_t *state,
                               int64_t now_us)
{
    uint16_t bg, border, fg;
    toast_colors(spr, state->tone, &bg, &border, &fg);
    (void)border;

    draw_message_bubble(spr, x, y, w, h, state->text, bg, fg, now_us);
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

    /* Sleep message: animação contínua, independente dos overlays de UI. */
    bool bubble = s_sleep_bubble_active;
    if (bubble) {
        int dirty_x = 0, dirty_y = 0, dirty_w = 0, dirty_h = 0;
        sleep_bubble_rect(now_us, &dirty_x, &dirty_y, &dirty_w, &dirty_h);
        if (s_sleep_bubble_was_active) {
            render_service_mark_dirty(s_bubble_prev_x, s_bubble_prev_y,
                                      s_bubble_prev_w, s_bubble_prev_h);
        }
        draw_sleep_bubble(spr, now_us, dirty_x, dirty_y, dirty_w, dirty_h);
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

    bool listening = s_listening_indicator_active;
    if (listening) {
        int dirty_x = 0, dirty_y = 0, dirty_w = 0, dirty_h = 0;
        listening_icon_rect(&dirty_x, &dirty_y, &dirty_w, &dirty_h);
        if (s_listening_was_active) {
            render_service_mark_dirty(s_listening_prev_x, s_listening_prev_y,
                                      s_listening_prev_w, s_listening_prev_h);
        }
        draw_listening_icon(spr, dirty_x, dirty_y, dirty_w, dirty_h, now_us);
        render_service_mark_dirty(dirty_x, dirty_y, dirty_w, dirty_h);
        s_listening_prev_x = dirty_x;
        s_listening_prev_y = dirty_y;
        s_listening_prev_w = dirty_w;
        s_listening_prev_h = dirty_h;
    } else if (s_listening_was_active) {
        render_service_mark_dirty(s_listening_prev_x, s_listening_prev_y,
                                  s_listening_prev_w, s_listening_prev_h);
    }
    s_listening_was_active = listening;

    bool timer_badge = s_timer_badge_active;
    if (timer_badge) {
        int dirty_x = 0, dirty_y = 0, dirty_w = 0, dirty_h = 0;
        timer_badge_rect(&dirty_x, &dirty_y, &dirty_w, &dirty_h);
        if (s_timer_badge_was_active) {
            render_service_mark_dirty(s_timer_badge_prev_x, s_timer_badge_prev_y,
                                      s_timer_badge_prev_w, s_timer_badge_prev_h);
        }
        draw_timer_badge(spr, dirty_x, dirty_y, dirty_w, dirty_h,
                         s_timer_badge_remaining_ms);
        render_service_mark_dirty(dirty_x, dirty_y, dirty_w, dirty_h);
        s_timer_badge_prev_x = dirty_x;
        s_timer_badge_prev_y = dirty_y;
        s_timer_badge_prev_w = dirty_w;
        s_timer_badge_prev_h = dirty_h;
    } else if (s_timer_badge_was_active) {
        render_service_mark_dirty(s_timer_badge_prev_x, s_timer_badge_prev_y,
                                  s_timer_badge_prev_w, s_timer_badge_prev_h);
    }
    s_timer_badge_was_active = timer_badge;

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
            draw_text_overlay(spr, x, y, w, h, &state, now_us);
            break;
        case OVERLAY_TOAST:
            draw_toast_overlay(spr, x, y, w, h, &state, now_us);
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

extern "C" void ui_overlay_clear_text(void)
{
    if (!s_initialized || !s_mutex) return;

    bool changed = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state.kind == OVERLAY_TEXT) {
        s_state.kind = OVERLAY_NONE;
        s_state.text[0] = '\0';
        s_state.expires_us = 0;
        changed = true;
    }
    xSemaphoreGive(s_mutex);

    if (changed) {
        render_service_force_full_refresh();
    }
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

extern "C" void ui_overlay_listening_set(bool enabled)
{
    s_listening_indicator_active = enabled;
    render_service_force_full_refresh();
}

extern "C" void ui_overlay_timer_badge_set(bool enabled, uint32_t remaining_ms)
{
    s_timer_badge_remaining_ms = remaining_ms;
    s_timer_badge_active = enabled;
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
