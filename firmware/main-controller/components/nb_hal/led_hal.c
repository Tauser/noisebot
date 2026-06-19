/*
 * led_hal.c — HAL para WS2812 via RMT (Layer 1)
 */

#include "led_hal.h"
#include "nb_hw_config.h"

#include "led_strip.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "nb_led_hal";

static led_strip_handle_t s_strip = NULL;

/* ── Init / deinit ───────────────────────────────────────────────────────── */

esp_err_t led_hal_init(void)
{
    if (s_strip != NULL) {
        ESP_LOGW(TAG, "led_hal_init chamado mais de uma vez — ignorado");
        return ESP_OK;
    }

    led_strip_config_t strip_cfg = {
        .strip_gpio_num        = NB_LED_PIN_DATA,
        .max_leds              = NB_LED_COUNT,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,  /* WS2812 usa GRB */
        .led_model             = LED_MODEL_WS2812,
        .flags.invert_out      = false,
    };

    led_strip_rmt_config_t rmt_cfg = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = 10 * 1000 * 1000,  /* 10 MHz — padrão WS2812 */
        .mem_block_symbols = 0,               /* padrão do driver       */
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device falhou: %s", esp_err_to_name(err));
        return err;
    }

    /* Apaga LEDs no boot. */
    err = led_strip_clear(s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_clear inicial falhou: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "led_hal OK — GPIO %d, %d LEDs, RMT 10MHz",
             NB_LED_PIN_DATA, NB_LED_COUNT);
    return ESP_OK;
}

void led_hal_deinit(void)
{
    if (s_strip == NULL) {
        return;
    }
    led_strip_clear(s_strip);
    led_strip_del(s_strip);
    s_strip = NULL;
}

/* ── Operações de pixel ──────────────────────────────────────────────────── */

esp_err_t led_hal_set_pixel(uint8_t idx, nb_led_color_t color)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (idx >= NB_LED_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    return led_strip_set_pixel(s_strip, idx, color.r, color.g, color.b);
}

esp_err_t led_hal_flush(void)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return led_strip_refresh(s_strip);
}

esp_err_t led_hal_clear(void)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = led_strip_clear(s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_hal_clear falhou: %s", esp_err_to_name(err));
    }
    return err;
}
