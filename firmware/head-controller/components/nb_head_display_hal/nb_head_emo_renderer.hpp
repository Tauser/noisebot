#pragma once

#include <LovyanGFX.hpp>
#include <stdint.h>

/*
 * Estado paramétrico de uma face (DM2.8): porte 1:1 de nb_face_state_t
 * (firmware/main-controller/components/services/expression_service/
 * expression_service.h), sem o campo color (todas as 10 expressões do main
 * usam TFT_WHITE; a cor chega por parâmetro em nb_head_emo_draw_face, igual
 * já fazia nb_head_emo_draw).
 */
struct nb_head_emo_face_t {
    float tl_l, tr_l, bl_l, br_l;
    float tl_r, tr_r, bl_r, br_r;
    float open_l, open_r;
    float y_l, y_r;
    float x_off;
    float rt_top, rb_bot;
    float cv_top, cv_bot;
    float squint_l, squint_r;
};

/* Tabela canônica (kExpressions), indexada por nb_expression_t. */
const nb_head_emo_face_t &nb_head_emo_get_face(uint8_t expression);

/* Interpolação linear, mesmo desenho de nb_face_state_lerp do main. */
void nb_head_emo_face_lerp(const nb_head_emo_face_t &a,
                           const nb_head_emo_face_t &b,
                           float t,
                           nb_head_emo_face_t &out);

/* Desenha uma face já resolvida (estática ou interpolada) + overlays. */
void nb_head_emo_draw_face(LGFX_Sprite &canvas,
                           const nb_head_emo_face_t &face,
                           int16_t gaze_x_milli,
                           int16_t gaze_y_milli,
                           uint16_t overlay_flags,
                           uint32_t color);

/* Atalho: resolve a face estática da tabela e desenha (sem interpolação). */
void nb_head_emo_draw(LGFX_Sprite &canvas,
                      uint8_t expression,
                      int16_t gaze_x_milli,
                      int16_t gaze_y_milli,
                      uint16_t overlay_flags,
                      uint32_t color);
