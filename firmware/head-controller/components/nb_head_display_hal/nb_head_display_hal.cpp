#include "nb_head_display_hal.h"

#include <algorithm>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "nb_head_blink.hpp"
#include "nb_head_emo_renderer.hpp"
#include "nb_head_lgfx_config.hpp"
#include "nb_head_status_icons.hpp"

#ifndef CONFIG_NB_HEAD_DISPLAY_HW_ENABLED
#define CONFIG_NB_HEAD_DISPLAY_HW_ENABLED 0
#endif

#define NB_HEAD_DISPLAY_MIN_SPIRAM_FREE (300U * 1024U)

static const char *TAG = "nb_head_disp_hal";
static NBHeadLGFX s_display;
static LGFX_Sprite s_frame(&s_display);
static bool s_ready;
static SemaphoreHandle_t s_display_mutex;
static nb_head_emo_face_t s_current_face = nb_head_emo_get_face(0U);

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

    const uint8_t level = command->brightness;
    const uint32_t color = ((uint32_t)level << 16) |
                           ((uint32_t)level << 8) | level;
    s_display.startWrite();
    s_frame.fillScreen(TFT_BLACK);
    s_current_face = nb_head_emo_get_face(command->expression);
    nb_head_emo_draw_face(s_frame, s_current_face,
                          command->gaze_x_milli, command->gaze_y_milli,
                          command->overlay_flags, color);
    s_frame.pushSprite(0, 0);
    s_display.endWrite();
    ESP_LOGD(TAG, "frame desenhado generation=%lu",
             (unsigned long)command->generation);
    xSemaphoreGive(s_display_mutex);
    return ESP_OK;
}

esp_err_t nb_head_display_hal_apply_blend(const nb_display_command_t *target,
                                          float face_t,
                                          bool force_redraw,
                                          uint32_t icon_bits)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!nb_display_command_is_valid(target, sizeof(*target))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (target->opcode != NB_DISPLAY_OP_SET_SCENE) {
        return ESP_ERR_INVALID_ARG;
    }
    const float t = std::clamp(face_t, 0.0f, 1.0f);

    /* DM2.9 -- blink autonomo do head. Calculado fora do mutex/SPI: se
     * nada esta animando (transicao concluida e nenhum olho em blink),
     * pula o redesenho/push SPI -- evita trafego SPI continuo a 50Hz
     * pra sempre, que desestabilizou o enlace dual-MCU em bancada
     * (READY <-> DEGRADED) quando testado pela primeira vez. */
    const nb_head_blink_mult_t blink =
        nb_head_blink_tick(esp_timer_get_time());
    if (t >= 1.0f && !blink.active && !force_redraw) {
        return ESP_OK;
    }

    if (xSemaphoreTake(s_display_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const nb_head_emo_face_t &target_face =
        nb_head_emo_get_face(target->expression);
    nb_head_emo_face_t blended;
    nb_head_emo_face_lerp(s_current_face, target_face, t, blended);
    blended.open_l *= blink.open_mult_l;
    blended.open_r *= blink.open_mult_r;

    const uint8_t level = target->brightness;
    const uint32_t color = ((uint32_t)level << 16) |
                           ((uint32_t)level << 8) | level;
    s_display.startWrite();
    s_frame.fillScreen(TFT_BLACK);
    nb_head_emo_draw_face(s_frame, blended,
                          target->gaze_x_milli, target->gaze_y_milli,
                          target->overlay_flags, color);
    nb_head_status_icons_draw(s_frame, icon_bits);
    s_frame.pushSprite(0, 0);
    s_display.endWrite();

    if (t >= 1.0f) {
        /* Finaliza no valor exato da tabela -- evita drift de float
         * acumulado entre transicoes sucessivas. */
        s_current_face = target_face;
    }
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
