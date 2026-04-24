/*
 * ui_overlay_service.h - Overlays visuais transitorios do NoiseBot.
 *
 * Layer 5. Registra uma layer no render_service e desenha pequenos
 * indicadores locais sem substituir a face/IDLE.
 */

#ifndef NB_UI_OVERLAY_SERVICE_H
#define NB_UI_OVERLAY_SERVICE_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ui_overlay_service_init(void);

void ui_overlay_show_volume(uint8_t percent, uint32_t duration_ms);
void ui_overlay_show_text(const char *text, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

#endif /* NB_UI_OVERLAY_SERVICE_H */
