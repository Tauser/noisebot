#include "nb_head_display_hal.h"

#include <algorithm>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nb_head_lgfx_config.hpp"

#ifndef CONFIG_NB_HEAD_DISPLAY_HW_ENABLED
#define CONFIG_NB_HEAD_DISPLAY_HW_ENABLED 0
#endif

#define NB_HEAD_DISPLAY_MIN_SPIRAM_FREE (300U * 1024U)

static const char *TAG = "nb_head_disp_hal";
static NBHeadLGFX s_display;
static bool s_ready;
static SemaphoreHandle_t s_display_mutex;

static esp_err_t reset_panel(void)
{
    const gpio_num_t reset_pin =
        static_cast<gpio_num_t>(NB_HEAD_PIN_DISP_RST);
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << reset_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        return err;
    }

    gpio_set_level(reset_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(reset_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(reset_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
    return ESP_OK;
}

static int gaze_offset(int16_t milli, int radius)
{
    return std::clamp((int)milli * radius / NB_DISPLAY_GAZE_MAX,
                      -radius, radius);
}

static void draw_face(int16_t gaze_x_milli,
                      int16_t gaze_y_milli,
                      uint8_t expression,
                      uint16_t accent)
{
    const int center_x = s_display.width() / 2;
    const int center_y = s_display.height() / 2;
    const int gaze_x = gaze_offset(gaze_x_milli, 12);
    const int gaze_y = gaze_offset(gaze_y_milli, 8);

    s_display.startWrite();
    s_display.fillScreen(TFT_BLACK);
    s_display.fillCircle(center_x - 55 + gaze_x,
                         center_y - 35 + gaze_y, 12, accent);
    s_display.fillCircle(center_x + 55 + gaze_x,
                         center_y - 35 + gaze_y, 12, accent);
    if ((expression & 1U) != 0U) {
        s_display.drawArc(center_x, center_y + 35, 48, 40,
                          20, 160, accent);
    } else {
        s_display.fillRect(center_x - 42, center_y + 40,
                           84, 4, accent);
    }
    s_display.endWrite();
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

    s_display_mutex = xSemaphoreCreateMutex();
    if (s_display_mutex == NULL) {
        ESP_LOGE(TAG, "falha ao criar mutex do display");
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t reset_err = reset_panel();
    if (reset_err != ESP_OK) {
        ESP_LOGE(TAG, "falha no reset fisico do ST7789: %s",
                 esp_err_to_name(reset_err));
        vSemaphoreDelete(s_display_mutex);
        s_display_mutex = nullptr;
        return reset_err;
    }

    if (!s_display.init()) {
        ESP_LOGE(TAG, "LovyanGFX falhou ao inicializar ST7789");
        vSemaphoreDelete(s_display_mutex);
        s_display_mutex = nullptr;
        return ESP_FAIL;
    }
    s_display.setRotation(0);
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
    if (xSemaphoreTake(s_display_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (command->opcode == NB_DISPLAY_OP_SET_POWER) {
        if (command->expression == NB_DISPLAY_POWER_OFF) {
            s_display.fillScreen(TFT_BLACK);
        }
        xSemaphoreGive(s_display_mutex);
        return ESP_OK;
    }
    if (command->opcode == NB_DISPLAY_OP_FORCE_REFRESH) {
        xSemaphoreGive(s_display_mutex);
        return ESP_OK;
    }

    const uint16_t accent =
        command->overlay_flags == 0U ? TFT_WHITE : TFT_CYAN;

    draw_face(command->gaze_x_milli,
              command->gaze_y_milli,
              command->expression,
              accent);
    ESP_LOGD(TAG, "frame desenhado generation=%lu",
             (unsigned long)command->generation);
    xSemaphoreGive(s_display_mutex);
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
