/*
 * nb_head_status_led.c — LED RGB onboard (WS2812, GPIO48) via RMT
 */

#include "nb_head_status_led.h"
#include "nb_hw_config_head.h"

#include "led_strip.h"
#include "esp_log.h"

static const char *TAG = "nb_head_led";

esp_err_t nb_head_status_led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num         = NB_HEAD_PIN_STATUS_LED,
        .max_leds               = 1,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model               = LED_MODEL_WS2812,
        .flags.invert_out       = false,
    };

    led_strip_rmt_config_t rmt_cfg = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags.with_dma    = false,
    };

    led_strip_handle_t strip = NULL;
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device falhou: %s", esp_err_to_name(err));
        return err;
    }

    err = led_strip_clear(strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_clear falhou: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "status LED OK — GPIO %d apagado", NB_HEAD_PIN_STATUS_LED);
    return ESP_OK;
}
