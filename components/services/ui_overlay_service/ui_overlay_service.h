/*
 * ui_overlay_service.h - Overlays visuais transitorios do NoiseBot.
 *
 * Layer 5. Registra uma layer no render_service e desenha pequenos
 * indicadores locais sem substituir a face/IDLE.
 */

#ifndef NB_UI_OVERLAY_SERVICE_H
#define NB_UI_OVERLAY_SERVICE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ui_overlay_service_init(void);

typedef enum {
    NB_UI_OVERLAY_INFO = 0,
    NB_UI_OVERLAY_SUCCESS,
    NB_UI_OVERLAY_WARNING,
    NB_UI_OVERLAY_ERROR,
} nb_ui_overlay_tone_t;

void ui_overlay_show_volume(uint8_t percent, uint32_t duration_ms);
void ui_overlay_show_text(const char *text, uint32_t duration_ms);
void ui_overlay_show_toast(const char *text, nb_ui_overlay_tone_t tone, uint32_t duration_ms);
void ui_overlay_clear(void);

/**
 * @brief Atualiza a posição visual atual dos olhos para ancorar overlays.
 *
 * Chamado pelo expression_service no render frame. Overlays que pertencem à
 * face devem derivar sua posição desses centros, em vez de usar coordenadas
 * absolutas fixas.
 */
void ui_overlay_set_eye_frame(int16_t left_cx, int16_t right_cx, int16_t eye_cy);

/**
 * @brief Ativa/desativa a bolha de sono animada (ciano, acima dos olhos).
 *
 * Quando ativa, desenha em loop uma bolha que infla e encolhe na área do
 * nariz. Deve ser ativada ao entrar em NB_STATE_SLEEPING e desativada ao sair.
 */
void ui_overlay_sleep_bubble_set(bool enabled);

#ifdef __cplusplus
}
#endif

#endif /* NB_UI_OVERLAY_SERVICE_H */
