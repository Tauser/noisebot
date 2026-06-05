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

typedef enum {
    NB_UI_STATUS_ICON_MIC_ACTIVE = 0,
    NB_UI_STATUS_ICON_MIC_BLOCKED,
    NB_UI_STATUS_ICON_CAMERA_ACTIVE,
    NB_UI_STATUS_ICON_SPEAKER_ACTIVE,
    NB_UI_STATUS_ICON_BRIDGE_CONNECTED,
    NB_UI_STATUS_ICON_BRIDGE_BUSY,
    NB_UI_STATUS_ICON_BRIDGE_OFFLINE,
    NB_UI_STATUS_ICON_WIFI_1,
    NB_UI_STATUS_ICON_WIFI_2,
    NB_UI_STATUS_ICON_WIFI_3,
    NB_UI_STATUS_ICON_WIFI_ALERT,
    NB_UI_STATUS_ICON_WIFI_UNAVAILABLE,
    NB_UI_STATUS_ICON_VOLUME_LOW,
    NB_UI_STATUS_ICON_VOLUME_HIGH,
    NB_UI_STATUS_ICON_VOLUME_MUTED,
    NB_UI_STATUS_ICON_VOLUME_OFF,
    NB_UI_STATUS_ICON_BATTERY_ABSENT,
    NB_UI_STATUS_ICON_BATTERY_EMPTY,
    NB_UI_STATUS_ICON_BATTERY_25,
    NB_UI_STATUS_ICON_BATTERY_50,
    NB_UI_STATUS_ICON_BATTERY_75,
    NB_UI_STATUS_ICON_BATTERY_FULL,
    NB_UI_STATUS_ICON_BATTERY_100,
    NB_UI_STATUS_ICON_BATTERY_CHARGING,
    NB_UI_STATUS_ICON_ALARM_ACTIVE,
    NB_UI_STATUS_ICON_LOCKED,
    NB_UI_STATUS_ICON_USER_IDENTIFYING,
    NB_UI_STATUS_ICON_WIFI_PASSWORD,
    NB_UI_STATUS_ICON_TEMP_ALERT,
    NB_UI_STATUS_ICON_CALENDAR_CLOCK,
    NB_UI_STATUS_ICON_COUNT,
} nb_ui_status_icon_t;

typedef struct {
    nb_ui_status_icon_t left_icon;  /* Use NB_UI_STATUS_ICON_COUNT para ocultar. */
    nb_ui_status_icon_t right_icon; /* Use NB_UI_STATUS_ICON_COUNT para ocultar. */
    const char *left_label;
    const char *center_label;
    const char *right_label;
} nb_ui_quick_status_t;

void ui_overlay_show_volume(uint8_t percent, uint32_t duration_ms);
void ui_overlay_show_text(const char *text, uint32_t duration_ms);
void ui_overlay_clear_text(void);
void ui_overlay_show_toast(const char *text, nb_ui_overlay_tone_t tone, uint32_t duration_ms);
void ui_overlay_clear(void);
void ui_overlay_status_icon_set(nb_ui_status_icon_t icon, bool enabled);
void ui_overlay_show_quick_status(const nb_ui_quick_status_t *status, uint32_t duration_ms);
void ui_overlay_listening_set(bool enabled);
void ui_overlay_camera_set(bool enabled);
void ui_overlay_timer_badge_set(bool enabled, uint32_t remaining_ms);

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
