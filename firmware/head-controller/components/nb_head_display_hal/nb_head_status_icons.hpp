#pragma once

#include <LovyanGFX.hpp>
#include <stdint.h>

/*
 * DM2.11 -- porte fiel do status rail de ui_overlay_service.cpp (main):
 * mesma lista de prioridade, posição (canto superior direito), cores por
 * ícone e escala 28x28 -> 24x24. `icon_bits` usa os mesmos bits ordinais
 * de nb_ui_status_icon_t (main) -- bit N = ícone com ordinal N.
 */
void nb_head_status_icons_draw(LGFX_Sprite &canvas, uint32_t icon_bits);
