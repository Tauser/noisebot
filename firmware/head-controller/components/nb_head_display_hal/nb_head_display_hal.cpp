#include "nb_head_display_hal.h"

#include <algorithm>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nb_head_lgfx_config.hpp"

#ifndef CONFIG_NB_HEAD_DISPLAY_HW_ENABLED
#define CONFIG_NB_HEAD_DISPLAY_HW_ENABLED 0
#endif

#define NB_HEAD_DISPLAY_MIN_SPIRAM_FREE (300U * 1024U)

static const char *TAG = "nb_head_disp_hal";
static NBHeadLGFX s_display;
static bool s_ready;

static int gaze_offset(int16_t milli, int radius)
{
    return std::clamp((int)milli * radius / NB_DISPLAY_GAZE_MAX,
                      -radius, radius);
}

extern "C" {

esp_err_t nb_head_display_hal_init(void)
{
    if (!CONFIG_NB_HEAD_DISPLAY_HW_ENABLED) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t free_before =
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_before < NB_HEAD_DISPLAY_MIN_SPIRAM_FREE) {
        ESP_LOGE(TAG, "PSRAM headroom insuficiente: %lu bytes",
                 (unsigned long)free_before);
        return ESP_ERR_NO_MEM;
    }

    if (!s_display.init()) {
        ESP_LOGE(TAG, "LovyanGFX falhou ao inicializar ST7789");
        return ESP_FAIL;
    }
    s_display.setRotation(1);
    s_display.fillScreen(TFT_BLACK);
    s_ready = true;

    ESP_LOGI(TAG,
             "ST7789 pronto SPI2=%dkHz %dx%d PSRAM=%lu bytes",
             NB_HEAD_DISP_SPI_FREQ_KHZ,
             s_display.width(),
             s_display.height(),
             (unsigned long)nb_head_display_hal_spiram_free());
    return ESP_OK;
}

esp_err_t nb_head_display_hal_apply(const nb_display_command_t *command)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!nb_display_command_is_valid(command, sizeof(*command))) {
        return ESP_ERR_INVALID_ARG;
    }

    if (command->opcode == NB_DISPLAY_OP_SET_POWER) {
        if (command->expression == NB_DISPLAY_POWER_OFF) {
            s_display.fillScreen(TFT_BLACK);
        }
        return ESP_OK;
    }
    if (command->opcode == NB_DISPLAY_OP_FORCE_REFRESH) {
        return ESP_OK;
    }

    const int width = s_display.width();
    const int height = s_display.height();
    const int center_x = width / 2;
    const int center_y = height / 2;
    const int gaze_x = gaze_offset(command->gaze_x_milli, 12);
    const int gaze_y = gaze_offset(command->gaze_y_milli, 8);
    const uint16_t accent =
        command->overlay_flags == 0U ? TFT_WHITE : TFT_CYAN;

    s_display.startWrite();
    s_display.fillScreen(TFT_BLACK);
    s_display.fillCircle(center_x - 55 + gaze_x,
                         center_y - 35 + gaze_y, 12, accent);
    s_display.fillCircle(center_x + 55 + gaze_x,
                         center_y - 35 + gaze_y, 12, accent);
    if ((command->expression & 1U) != 0U) {
        s_display.drawArc(center_x, center_y + 35, 48, 40,
                          20, 160, accent);
    } else {
        s_display.drawFastHLine(center_x - 42, center_y + 42,
                                84, accent);
    }
    s_display.endWrite();
    s_display.setBrightness(command->brightness);
    return ESP_OK;
}

bool nb_head_display_hal_is_ready(void)
{
    return s_ready;
}

uint32_t nb_head_display_hal_spiram_free(void)
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

}
