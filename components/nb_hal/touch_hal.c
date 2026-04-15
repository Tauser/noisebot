/*
 * touch_hal.c — HAL para touch capacitivo ESP32-S3 (Layer 1)
 */

#include "touch_hal.h"
#include "nb_hw_config.h"

#include "driver/touch_pad.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "nb_touch_hal";

#define CALIB_SAMPLES    30
#define CALIB_DELAY_MS   20

static uint32_t s_baseline  = 0;
static bool     s_initialized = false;

esp_err_t nb_touch_hal_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "nb_touch_hal_init chamado mais de uma vez");
        return ESP_OK;
    }

    esp_err_t err;

    err = touch_pad_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch_pad_init falhou: %s", esp_err_to_name(err));
        return err;
    }

    /* FSM automático: mede continuamente sem intervenção do SW. */
    err = touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch_pad_set_fsm_mode falhou: %s", esp_err_to_name(err));
        return err;
    }

    /* Configura o pad com parâmetros padrão. */
    err = touch_pad_config(NB_TOUCH_PAD_NUM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch_pad_config(%d) falhou: %s",
                 NB_TOUCH_PAD_NUM, esp_err_to_name(err));
        return err;
    }

    /* Inicia o FSM de medição. */
    err = touch_pad_fsm_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch_pad_fsm_start falhou: %s", esp_err_to_name(err));
        return err;
    }

    /* Aguarda o periférico estabilizar antes de calibrar.
     * 500ms é suficiente — o threshold alto (≥30% acima do baseline) absorve
     * qualquer drift de capacitância sem precisar de settling longo. */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Calibração: média simples de CALIB_SAMPLES leituras. */
    uint64_t sum = 0;
    for (int i = 0; i < CALIB_SAMPLES; i++) {
        uint32_t raw = 0;
        touch_pad_read_raw_data(NB_TOUCH_PAD_NUM, &raw);
        sum += raw;
        vTaskDelay(pdMS_TO_TICKS(CALIB_DELAY_MS));
    }
    s_baseline = (uint32_t)(sum / CALIB_SAMPLES);

    if (s_baseline == 0) {
        ESP_LOGE(TAG, "baseline zero — periférico nao respondeu");
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "touch_hal OK — pad %d, GPIO %d, baseline %lu",
             NB_TOUCH_PAD_NUM, NB_TOUCH_PIN, (unsigned long)s_baseline);
    return ESP_OK;
}

esp_err_t nb_touch_hal_read(uint32_t *raw)
{
    if (!s_initialized || raw == NULL) return ESP_ERR_INVALID_STATE;
    return touch_pad_read_raw_data(NB_TOUCH_PAD_NUM, raw);
}

uint32_t nb_touch_hal_get_baseline(void)
{
    return s_baseline;
}

void nb_touch_hal_deinit(void)
{
    if (!s_initialized) return;
    touch_pad_fsm_stop();
    touch_pad_deinit();
    s_initialized = false;
}
