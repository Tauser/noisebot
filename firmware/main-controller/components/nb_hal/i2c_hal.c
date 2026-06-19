/*
 * i2c_hal.c — HAL de barramento I2C compartilhado (Layer 1)
 */

#include "i2c_hal.h"
#include "esp_log.h"

static const char *TAG = "nb_i2c_hal";

static i2c_master_bus_handle_t s_bus_handle = NULL;
static bool s_initialized = false;

esp_err_t nb_i2c_hal_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "barramento já inicializado");
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port            = NB_I2C_PORT,
        .sda_io_num          = NB_I2C_PIN_SDA,
        .scl_io_num          = NB_I2C_PIN_SCL,
        .clk_source          = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt   = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "falha ao criar bus I2C: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "bus I2C iniciado — SDA=%d SCL=%d freq=%dkHz",
             NB_I2C_PIN_SDA, NB_I2C_PIN_SCL, NB_I2C_FREQ_HZ / 1000);
    return ESP_OK;
}

i2c_master_bus_handle_t nb_i2c_hal_get_bus(void)
{
    return s_bus_handle;
}

bool nb_i2c_hal_is_ready(void)
{
    return s_initialized;
}

esp_err_t nb_i2c_hal_scan(uint8_t *found, size_t max_found, size_t *count)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "scan chamado antes de init");
        return ESP_ERR_INVALID_STATE;
    }

    size_t found_count = 0;
    ESP_LOGI(TAG, "escaneando barramento I2C...");

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        esp_err_t ret = i2c_master_probe(s_bus_handle, addr, 10);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "  dispositivo em 0x%02X", addr);
            if (found != NULL && found_count < max_found) {
                found[found_count] = addr;
            }
            found_count++;
        }
    }

    if (found_count == 0) {
        ESP_LOGW(TAG, "nenhum dispositivo I2C encontrado");
    } else {
        ESP_LOGI(TAG, "scan concluído: %zu dispositivo(s)", found_count);
    }

    if (count != NULL) {
        *count = found_count;
    }
    return ESP_OK;
}

void nb_i2c_hal_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    i2c_del_master_bus(s_bus_handle);
    s_bus_handle = NULL;
    s_initialized = false;

    ESP_LOGI(TAG, "bus I2C desinicializado");
}
