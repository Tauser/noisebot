#include "nb_head_status_icons.hpp"

#include <cmath>

#include "esp_timer.h"
#include "nb_head_status_icon_assets.h"

namespace {

/*
 * Porte fiel de ui_overlay_service.cpp (main) -- nb_ui_status_icon_t não
 * pode ser incluído direto (componente separado, projeto ESP-IDF
 * separado), então os ordinais são replicados aqui. Têm que casar
 * exatamente com firmware/main-controller/.../ui_overlay_service.h --
 * qualquer mudança lá precisa ser espelhada aqui também.
 */
enum NbUiStatusIcon {
    kMicActive = 0,
    kMicBlocked,
    kCameraActive,
    kSpeakerActive,
    kBridgeConnected,
    kBridgeBusy,
    kBridgeOffline,
    kWifi1,
    kWifi2,
    kWifi3,
    kWifiAlert,
    kWifiUnavailable,
    kVolumeLow,
    kVolumeHigh,
    kVolumeMuted,
    kVolumeOff,
    kBatteryAbsent,
    kBatteryEmpty,
    kBattery25,
    kBattery50,
    kBattery75,
    kBatteryFull,
    kBattery100,
    kBatteryCharging,
    kAlarmActive,
    kLocked,
    kUserIdentifying,
    kWifiPassword,
    kTempAlert,
    kCalendarClock,
    kStatusIconCount,
};

/* Porte fiel de status_icon_asset() (main). */
const nb_ui_overlay_icon_t *status_icon_asset(int icon)
{
    switch (icon) {
        case kMicActive:        return &NB_UI_OVERLAY_ICON_MICROFONE;
        case kMicBlocked:       return &NB_UI_OVERLAY_ICON_MICROFONE_BLOQUEADO;
        case kCameraActive:     return &NB_UI_OVERLAY_ICON_CAMERA;
        case kSpeakerActive:    return &NB_UI_OVERLAY_ICON_VOLUME;
        case kBridgeConnected:  return &NB_UI_OVERLAY_ICON_SERVER_ON;
        case kBridgeBusy:       return &NB_UI_OVERLAY_ICON_SERVER_RELOAD;
        case kBridgeOffline:    return &NB_UI_OVERLAY_ICON_SERVER_OFF;
        case kWifi1:            return &NB_UI_OVERLAY_ICON_WIFI_1;
        case kWifi2:            return &NB_UI_OVERLAY_ICON_WI_FI_2;
        case kWifi3:            return &NB_UI_OVERLAY_ICON_WI_FI_3;
        case kWifiAlert:        return &NB_UI_OVERLAY_ICON_WI_FI_ALERTA;
        case kWifiUnavailable:  return &NB_UI_OVERLAY_ICON_WI_FI_INDISPONIVEL;
        case kVolumeLow:        return &NB_UI_OVERLAY_ICON_VOLUME_BAIXO;
        case kVolumeHigh:       return &NB_UI_OVERLAY_ICON_VOLUME;
        case kVolumeMuted:      return &NB_UI_OVERLAY_ICON_VOLUME_MUDO;
        case kVolumeOff:        return &NB_UI_OVERLAY_ICON_VOLUME_DESLIGADO;
        case kBatteryAbsent:    return &NB_UI_OVERLAY_ICON_BATERIA_AUSENTE;
        case kBatteryEmpty:     return &NB_UI_OVERLAY_ICON_BATERIA_VAZIA;
        case kBattery25:        return &NB_UI_OVERLAY_ICON_BATERIA_QUARTO;
        case kBattery50:        return &NB_UI_OVERLAY_ICON_BATERIA_METADE;
        case kBattery75:        return &NB_UI_OVERLAY_ICON_BATERIA_TRES_QUARTOS;
        case kBatteryFull:      return &NB_UI_OVERLAY_ICON_BATERIA_CHEIA;
        case kBattery100:       return &NB_UI_OVERLAY_ICON_BATERIA_100;
        case kBatteryCharging:  return &NB_UI_OVERLAY_ICON_BATERIA_CARREGANDO;
        case kAlarmActive:      return &NB_UI_OVERLAY_ICON_DESPERTADOR;
        case kLocked:           return &NB_UI_OVERLAY_ICON_BLOQUEAR;
        case kUserIdentifying:  return &NB_UI_OVERLAY_ICON_IDENTIFICACAO_USUARIO;
        case kWifiPassword:     return &NB_UI_OVERLAY_ICON_SENHA_DO_WIFI;
        case kTempAlert:        return &NB_UI_OVERLAY_ICON_ALERTA_DE_ALTA_TEMPERATURA;
        case kCalendarClock:    return &NB_UI_OVERLAY_ICON_RELOGIO_CALENDARIO;
        default:                return nullptr;
    }
}

constexpr float kBubblePiF = 3.14159265358979323846f;

uint32_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Porte fiel de status_icon_color() (main). */
uint32_t status_icon_color(int icon, int64_t now_us)
{
    const float phase = (float)((now_us / 1000LL) % 1600LL) / 1600.0f;
    const float pulse = 0.5f + (0.5f * sinf(phase * 2.0f * kBubblePiF));

    switch (icon) {
        case kMicBlocked:
        case kVolumeMuted:
        case kVolumeOff:
        case kBatteryAbsent:
            return rgb(244, 174, 82);
        case kWifiAlert:
        case kWifiUnavailable:
        case kBridgeOffline:
        case kBatteryEmpty:
        case kTempAlert:
            return rgb(246, 93, 83);
        case kBatteryCharging:
            return rgb(92, 232, 141);
        case kMicActive:
        case kCameraActive:
        case kSpeakerActive: {
            const uint8_t glow = (uint8_t)(178.0f + (pulse * 48.0f));
            return rgb(118, glow, 250);
        }
        default:
            return rgb(226, 238, 246);
    }
}

/* Porte fiel de draw_icon_mask() (main, com escala). */
void draw_icon_mask_scaled(LGFX_Sprite &canvas, const nb_ui_overlay_icon_t *icon,
                           int x, int y, int w, int h, uint32_t color)
{
    if (icon == nullptr || icon->mask == nullptr || w <= 0 || h <= 0) {
        return;
    }
    for (int dy = 0; dy < h; dy++) {
        const uint8_t py = (uint8_t)((dy * (int)icon->height) / h);
        const uint8_t *row = &icon->mask[(size_t)py * (size_t)icon->stride];
        for (int dx = 0; dx < w; dx++) {
            const uint8_t px = (uint8_t)((dx * (int)icon->width) / w);
            const uint8_t bit = (uint8_t)(0x80U >> (px & 0x07U));
            if ((row[px >> 3] & bit) != 0U) {
                canvas.drawPixel(x + dx, y + dy, color);
            }
        }
    }
}

bool status_icon_enabled(uint32_t flags, int icon)
{
    if (icon < 0 || icon >= kStatusIconCount) {
        return false;
    }
    return (flags & (1UL << (uint32_t)icon)) != 0U;
}

/* Mesmas constantes de ui_overlay_service.cpp (main). */
constexpr int kStatusRailSlotW = 24;
constexpr int kStatusRailSlotH = 24;
constexpr int kStatusRailMargin = 8;
constexpr int kStatusIconGap = 6;
constexpr int kStatusRailMaxIcons = 4;

/* Mesma ordem de PRIORITY[] em draw_status_rail() (main). */
constexpr int kPriority[] = {
    kTempAlert,       kBatteryEmpty,    kBridgeOffline,   kMicBlocked,
    kCameraActive,    kMicActive,       kSpeakerActive,   kBridgeBusy,
    kWifiAlert,       kWifiUnavailable, kWifi1,           kWifi2,
    kWifi3,           kVolumeMuted,     kVolumeOff,       kVolumeLow,
    kVolumeHigh,      kBatteryAbsent,   kBatteryCharging, kBattery25,
    kBattery50,       kBattery75,       kBatteryFull,     kBattery100,
    kAlarmActive,     kLocked,          kUserIdentifying, kWifiPassword,
    kCalendarClock,
};

}  // namespace

void nb_head_status_icons_draw(LGFX_Sprite &canvas, uint32_t icon_bits)
{
    /* Porte fiel de status_rail_rect() (main). */
    int dw = canvas.width();
    if (dw <= 0) {
        dw = 320;
    }
    const int rail_w = (kStatusRailMaxIcons * kStatusRailSlotW) +
                       ((kStatusRailMaxIcons - 1) * kStatusIconGap);
    const int rail_x = dw - kStatusRailMargin - rail_w;
    const int rail_y = kStatusRailMargin;

    const int64_t now_us = esp_timer_get_time();
    int slot = 0;
    for (int icon : kPriority) {
        if (!status_icon_enabled(icon_bits, icon)) {
            continue;
        }
        const nb_ui_overlay_icon_t *asset = status_icon_asset(icon);
        if (asset == nullptr) {
            continue;
        }
        const int x = rail_x + rail_w - kStatusRailSlotW -
                     (slot * (kStatusRailSlotW + kStatusIconGap));
        draw_icon_mask_scaled(canvas, asset, x, rail_y, kStatusRailSlotW,
                              kStatusRailSlotH,
                              status_icon_color(icon, now_us));
        slot++;
        if (slot >= kStatusRailMaxIcons) {
            break;
        }
    }
}
