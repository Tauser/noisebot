#pragma once

#include <LovyanGFX.hpp>
#include <stdint.h>

#include "nb_display_protocol.h"

/*
 * DM2.11 (fatia minima) -- so MIC_BLOCKED por ora. Mascara 1-bit copiada
 * de firmware/main-controller/components/services/ui_overlay_service/
 * icons/generated/nb_ui_overlay_icons.h (NB_UI_OVERLAY_ICON_MICROFONE_
 * BLOQUEADO), formato 28x28 PBM gerado por generate_overlay_icons.py.
 * Mesmo desenho de draw_icon_mask() do ui_overlay_service.cpp do main.
 */
void nb_head_status_icons_draw(LGFX_Sprite &canvas, uint32_t icon_bits);
