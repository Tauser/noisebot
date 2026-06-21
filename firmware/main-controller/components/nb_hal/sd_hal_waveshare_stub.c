/*
 * sd_hal_waveshare_stub.c — stub do microSD no perfil Waveshare
 */

#include "sd_hal.h"

#include "esp_log.h"

static const char *TAG = "nb_sd_stub";
static bool s_logged = false;

static void log_once(void)
{
    if (!s_logged) {
        ESP_LOGW(TAG, "SD desabilitado no perfil Waveshare");
        s_logged = true;
    }
}

esp_err_t sd_hal_init(void)
{
    log_once();
    return ESP_ERR_NOT_SUPPORTED;
}

void sd_hal_deinit(void)
{
    log_once();
}

bool sd_hal_is_mounted(void)
{
    return false;
}

esp_err_t sd_hal_try_remount(void)
{
    log_once();
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sd_hal_get_free_bytes(uint64_t *out_free_bytes)
{
    if (out_free_bytes != NULL) {
        *out_free_bytes = 0;
    }
    log_once();
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sd_hal_get_total_bytes(uint64_t *out_total_bytes)
{
    if (out_total_bytes != NULL) {
        *out_total_bytes = 0;
    }
    log_once();
    return ESP_ERR_NOT_SUPPORTED;
}
