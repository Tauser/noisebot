#include "nb_head_emo_renderer.hpp"

#include <algorithm>
#include <cmath>

#include "nb_display_protocol.h"

namespace {

struct Face {
    float tl_l, tr_l, bl_l, br_l;
    float tl_r, tr_r, bl_r, br_r;
    float open_l, open_r;
    float y_l, y_r;
    float x_off;
    float rt_top, rb_bot;
    float cv_top, cv_bot;
    float squint_l, squint_r;
};

constexpr Face kExpressions[NB_DISPLAY_EXPRESSION_COUNT] = {
    {0,0,0,0, 0,0,0,0, .88f,.88f, 0,0, .60f, .64f,.64f, 0,0, 0,0},
    {0,0,.72f,.72f, 0,0,.72f,.72f, .41f,.41f, 0,0, .60f,
     .27f,.52f, 1,-1, .22f,.22f},
    {0,0,0,0, .19f,0,0,0, .82f,.96f, 0,0, .60f,
     .64f,.64f, 0,0, 0,0},
    {0,0,0,0, 0,0,0,0, .14f,.14f, -.55f,-.55f, .60f,
     .19f,1, -.38f,.45f, .51f,.51f},
    {0,.30f,0,0, .30f,0,0,0, .80f,.80f, 0,0, .60f,
     .64f,.64f, .30f,.05f, .10f,.10f},
    {0,.38f,0,0, .38f,0,0,0, .86f,.86f, .10f,.10f, .60f,
     .64f,.64f, -.44f,.05f, .38f,.38f},
    {0,0,0,0, 0,0,0,0, 1,1, 0,0, .60f, .64f,.64f, 0,0, 0,0},
    {.70f,.13f,0,.44f, 0,.70f,.44f,0, .68f,.68f, .20f,.20f,
     .60f, .64f,.64f, -.16f,.08f, .08f,.08f},
    {0,.28f,0,0, .28f,0,0,0, .88f,.88f, -.18f,-.18f, .60f,
     .64f,.64f, .55f,.10f, 0,0},
    {0,.88f,.93f,.60f, .88f,0,.60f,.93f, .82f,.82f, 0,0,
     .60f, .26f,.14f, .06f,-.10f, 0,0},
};

constexpr int16_t kBaseLeftX = 96;
constexpr int16_t kBaseRightX = 224;
constexpr int16_t kEyeBaseY = 122;
constexpr int16_t kHalfWidth = 46;
constexpr float kHalfHeight = 46.0f;
constexpr float kYTravel = 32.0f;
constexpr float kXOffsetTravel = 18.0f;
constexpr float kCurve = 10.0f;
constexpr float kGazeXTravel = 14.0f;
constexpr float kGazeYLimit = .55f;

uint32_t blend_black(uint32_t color, float alpha)
{
    const uint8_t r =
        static_cast<uint8_t>(((color >> 16) & 0xffU) * alpha);
    const uint8_t g =
        static_cast<uint8_t>(((color >> 8) & 0xffU) * alpha);
    const uint8_t b =
        static_cast<uint8_t>((color & 0xffU) * alpha);
    return (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | b;
}

void draw_eye(LGFX_Sprite &canvas,
              int16_t cx,
              float cy,
              float open,
              float tl,
              float tr,
              float bl,
              float br,
              float squint,
              float round_top,
              float round_bottom,
              float curve_top,
              float curve_bottom,
              uint32_t color)
{
    if (open < .03f) {
        canvas.drawFastHLine(cx - kHalfWidth, static_cast<int16_t>(cy),
                             kHalfWidth * 2, color);
        return;
    }

    const float hh = open * kHalfHeight;
    const float y_tl = cy - hh * (1.0f - tl);
    const float y_tr = cy - hh * (1.0f - tr);
    const float y_bl = cy + hh * (1.0f - bl);
    const float y_br = cy + hh * (1.0f - br);
    const float squint_px = squint * hh * .65f;
    const float r_top = std::min(round_top * kHalfWidth * .68f, hh);
    const float r_bottom =
        std::min(round_bottom * kHalfWidth * .68f, hh);

    for (int16_t x = cx - kHalfWidth; x <= cx + kHalfWidth; ++x) {
        const float t =
            static_cast<float>(x - (cx - kHalfWidth)) /
            static_cast<float>(kHalfWidth * 2);
        const float parabola = 4.0f * t * (1.0f - t);
        float top = y_tl + t * (y_tr - y_tl) -
                    curve_top * kCurve * parabola;
        float bottom = y_bl + t * (y_br - y_bl) +
                       curve_bottom * kCurve * parabola;
        const float edge = std::min(
            static_cast<float>(x - (cx - kHalfWidth)),
            static_cast<float>((cx + kHalfWidth) - x));

        if (r_top > 0 && edge < r_top) {
            const float q = 1.0f - edge / r_top;
            top += r_top * (1.0f - std::sqrt(1.0f - q * q));
        }
        if (r_bottom > 0 && edge < r_bottom) {
            const float q = 1.0f - edge / r_bottom;
            bottom -= r_bottom * (1.0f - std::sqrt(1.0f - q * q));
        }

        top += squint_px;
        if (top >= bottom - .5f) {
            continue;
        }

        const int16_t top_full = static_cast<int16_t>(std::ceil(top));
        const int16_t bottom_full =
            static_cast<int16_t>(std::floor(bottom));
        const float alpha_top = static_cast<float>(top_full) - top;
        const float alpha_bottom =
            bottom - static_cast<float>(bottom_full);

        if (alpha_top > .04f) {
            canvas.drawPixel(x, top_full - 1,
                             blend_black(color, alpha_top));
        }
        if (bottom_full >= top_full) {
            canvas.drawFastVLine(x, top_full,
                                 bottom_full - top_full + 1, color);
        }
        if (alpha_bottom > .04f) {
            canvas.drawPixel(x, bottom_full + 1,
                             blend_black(color, alpha_bottom));
        }
    }
}

}  // namespace

void nb_head_emo_draw(LGFX_Sprite &canvas,
                      uint8_t expression,
                      int16_t gaze_x_milli,
                      int16_t gaze_y_milli,
                      uint32_t color)
{
    const Face &face = kExpressions[expression];
    const float gaze_x = std::clamp(
        static_cast<float>(gaze_x_milli) / NB_DISPLAY_GAZE_MAX,
        -1.0f, 1.0f);
    const float gaze_y = std::clamp(
        static_cast<float>(gaze_y_milli) / NB_DISPLAY_GAZE_MAX,
        -kGazeYLimit, kGazeYLimit);
    const int16_t gaze_shift =
        static_cast<int16_t>(std::round(gaze_x * kGazeXTravel));
    const int16_t x_offset =
        static_cast<int16_t>(std::round(face.x_off * kXOffsetTravel));
    const int16_t left_x = kBaseLeftX + x_offset + gaze_shift;
    const int16_t right_x = kBaseRightX - x_offset + gaze_shift;
    const float left_y = kEyeBaseY +
                         (1.0f - face.open_l) * kHalfHeight +
                         std::clamp(face.y_l + gaze_y,
                                    -kGazeYLimit, kGazeYLimit) * kYTravel;
    const float right_y = kEyeBaseY +
                          (1.0f - face.open_r) * kHalfHeight +
                          std::clamp(face.y_r + gaze_y,
                                     -kGazeYLimit, kGazeYLimit) * kYTravel;

    draw_eye(canvas, left_x, left_y, face.open_l,
             face.tl_l, face.tr_l, face.bl_l, face.br_l,
             face.squint_l, face.rt_top, face.rb_bot,
             face.cv_top, face.cv_bot, color);
    draw_eye(canvas, right_x, right_y, face.open_r,
             face.tl_r, face.tr_r, face.bl_r, face.br_r,
             face.squint_r, face.rt_top, face.rb_bot,
             face.cv_top, face.cv_bot, color);
}
