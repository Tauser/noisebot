#pragma once

#include <LovyanGFX.hpp>
#include <stdint.h>

void nb_head_emo_draw(LGFX_Sprite &canvas,
                      uint8_t expression,
                      int16_t gaze_x_milli,
                      int16_t gaze_y_milli,
                      uint32_t color);
