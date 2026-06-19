/*
 * display_hal.cpp — Implementação do HAL do display do NoiseBot
 */

#include "display_hal.h"
#include "display_lgfx_config.hpp"

#include "esp_log.h"
#include "esp_heap_caps.h"

#define TAG "nb_disp"

static NB_LGFX s_lgfx;
static bool    s_initialized = false;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static inline LGFX_Sprite *to_sprite(nb_display_sprite_t h)
{
    return static_cast<LGFX_Sprite *>(h);
}

/* ── API (extern "C") ────────────────────────────────────────────────────── */

extern "C" {

esp_err_t display_hal_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "display_hal_init chamado mais de uma vez");
        return ESP_FAIL;
    }

    s_lgfx.init();
    s_lgfx.fillScreen(TFT_BLACK);

    s_initialized = true;

#if defined(NB_DISP_DRIVER_ILI9342)
    const char *driver = "ILI9342";
#else
    const char *driver = "ST7789";
#endif

    ESP_LOGI(TAG, "Display inicializado — driver=%s, %dx%d",
             driver, s_lgfx.width(), s_lgfx.height());

    return ESP_OK;
}

void display_hal_set_brightness(uint8_t level)
{
    /*
     * LovyanGFX: setBrightness() é no-op se nenhum Light foi configurado
     * no painel (caso do ST7789 sem BL). Chamada segura em ambos os casos.
     */
    s_lgfx.setBrightness(level);
}

void display_hal_fill(uint16_t color_rgb565)
{
    s_lgfx.fillScreen(color_rgb565);
}

int display_hal_width(void)
{
    return s_lgfx.width();
}

int display_hal_height(void)
{
    return s_lgfx.height();
}

nb_display_sprite_t display_hal_sprite_create(int w, int h)
{
    LGFX_Sprite *sprite = new LGFX_Sprite(&s_lgfx);
    if (!sprite) {
        ESP_LOGE(TAG, "sprite: falha ao alocar objeto LGFX_Sprite");
        return nullptr;
    }

    sprite->setPsram(true);

    if (!sprite->createSprite(w, h)) {
        ESP_LOGE(TAG, "sprite: createSprite(%d, %d) falhou — PSRAM livre: %u bytes",
                 w, h,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        delete sprite;
        return nullptr;
    }

    ESP_LOGD(TAG, "sprite criado %dx%d em PSRAM (~%u bytes)",
             w, h, (unsigned)(w * h * 2));

    return static_cast<nb_display_sprite_t>(sprite);
}

void display_hal_sprite_push(nb_display_sprite_t sprite, int x, int y)
{
    if (!sprite) return;
    to_sprite(sprite)->pushSprite(x, y);
}

void display_hal_sprite_delete(nb_display_sprite_t sprite)
{
    if (!sprite) return;
    LGFX_Sprite *s = to_sprite(sprite);
    s->deleteSprite();
    delete s;
}

void *display_hal_get_lgfx(void)
{
    return static_cast<void *>(&s_lgfx);
}

} /* extern "C" */
