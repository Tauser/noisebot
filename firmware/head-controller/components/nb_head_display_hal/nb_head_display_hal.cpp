#include "nb_head_display_hal.h"

#include <algorithm>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nb_head_emo_renderer.hpp"
#include "nb_head_lgfx_config.hpp"

#ifndef CONFIG_NB_HEAD_DISPLAY_HW_ENABLED
#define CONFIG_NB_HEAD_DISPLAY_HW_ENABLED 0
#endif

#define NB_HEAD_DISPLAY_MIN_SPIRAM_FREE (300U * 1024U)

static const char *TAG = "nb_head_disp_hal";
static NBHeadLGFX s_display;
static LGFX_Sprite s_frame(&s_display);
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

static void draw_face(int16_t gaze_x_milli,
                      int16_t gaze_y_milli,
                      uint8_t expression,
                      uint16_t accent)
{
    s_display.startWrite();
    s_frame.fillScreen(TFT_BLACK);
    nb_head_emo_draw(s_frame, expression,
                     gaze_x_milli, gaze_y_milli, accent);
    s_frame.pushSprite(0, 0);
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
    s_frame.setColorDepth(16);
    s_frame.setPsram(true);
    if (!s_frame.createSprite(s_display.width(), s_display.height())) {
        ESP_LOGE(TAG, "falha ao alocar framebuffer %dx%d em PSRAM",
                 s_display.width(), s_display.height());
        vSemaphoreDelete(s_display_mutex);
        s_display_mutex = nullptr;
        return ESP_ERR_NO_MEM;
    }
    if (nb_head_display_hal_spiram_free() <
        NB_HEAD_DISPLAY_MIN_SPIRAM_FREE) {
        ESP_LOGE(TAG, "PSRAM headroom insuficiente apos framebuffer: %lu",
                 (unsigned long)nb_head_display_hal_spiram_free());
        s_frame.deleteSprite();
        vSemaphoreDelete(s_display_mutex);
        s_display_mutex = nullptr;
        return ESP_ERR_NO_MEM;
    }
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

    draw_face(command->gaze_x_milli,
              command->gaze_y_milli,
              command->expression,
              TFT_WHITE);
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
