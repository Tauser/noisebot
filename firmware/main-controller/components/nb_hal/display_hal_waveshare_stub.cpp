/*
 * display_hal_waveshare_stub.cpp — stub do display no perfil Waveshare
 */

#include "display_hal.h"

#include "esp_log.h"

#define TAG "nb_disp_stub"

static bool s_logged = false;

static void log_once(void)
{
    if (!s_logged) {
        ESP_LOGW(TAG, "display desabilitado no perfil Waveshare");
        s_logged = true;
    }
}

extern "C" {

esp_err_t display_hal_init(void)
{
    log_once();
    return ESP_ERR_NOT_SUPPORTED;
}

void display_hal_set_brightness(uint8_t level)
{
    (void)level;
    log_once();
}

void display_hal_fill(uint16_t color_rgb565)
{
    (void)color_rgb565;
    log_once();
}

int display_hal_width(void)
{
    return 0;
}

int display_hal_height(void)
{
    return 0;
}

nb_display_sprite_t display_hal_sprite_create(int w, int h)
{
    (void)w;
    (void)h;
    log_once();
    return nullptr;
}

void display_hal_sprite_push(nb_display_sprite_t sprite, int x, int y)
{
    (void)sprite;
    (void)x;
    (void)y;
    log_once();
}

void display_hal_sprite_delete(nb_display_sprite_t sprite)
{
    (void)sprite;
}

void *display_hal_get_lgfx(void)
{
    log_once();
    return nullptr;
}

} /* extern "C" */
