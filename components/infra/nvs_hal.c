/*
 * nvs_hal.c — Implementação do wrapper NVS do NoiseBot
 */

#include "nvs_hal.h"
#include "esp_log.h"

#define TAG "nb_nvs"

/* ── Open / Close / Commit ───────────────────────────────────────────────── */

esp_err_t nvs_hal_open(const char *ns, nvs_open_mode_t mode,
                       nvs_handle_t *out_handle)
{
    esp_err_t err = nvs_open(ns, mode, out_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(\"%s\") falhou: %s", ns, esp_err_to_name(err));
    }
    return err;
}

void nvs_hal_close(nvs_handle_t handle)
{
    nvs_close(handle);
}

esp_err_t nvs_hal_commit(nvs_handle_t handle)
{
    esp_err_t err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit falhou: %s", esp_err_to_name(err));
    }
    return err;
}

bool nvs_hal_key_exists(nvs_handle_t handle, const char *key)
{
    nvs_type_t type;
    return nvs_find_key(handle, key, &type) == ESP_OK;
}

/* ── Leitura com default ─────────────────────────────────────────────────── */

uint8_t nvs_hal_get_u8(nvs_handle_t h, const char *key, uint8_t default_val)
{
    uint8_t val = default_val;
    esp_err_t err = nvs_get_u8(h, key, &val);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_u8(\"%s\"): %s", key, esp_err_to_name(err));
    }
    return val;
}

uint16_t nvs_hal_get_u16(nvs_handle_t h, const char *key, uint16_t default_val)
{
    uint16_t val = default_val;
    esp_err_t err = nvs_get_u16(h, key, &val);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_u16(\"%s\"): %s", key, esp_err_to_name(err));
    }
    return val;
}

uint32_t nvs_hal_get_u32(nvs_handle_t h, const char *key, uint32_t default_val)
{
    uint32_t val = default_val;
    esp_err_t err = nvs_get_u32(h, key, &val);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_u32(\"%s\"): %s", key, esp_err_to_name(err));
    }
    return val;
}

int8_t nvs_hal_get_i8(nvs_handle_t h, const char *key, int8_t default_val)
{
    int8_t val = default_val;
    esp_err_t err = nvs_get_i8(h, key, &val);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_i8(\"%s\"): %s", key, esp_err_to_name(err));
    }
    return val;
}

int16_t nvs_hal_get_i16(nvs_handle_t h, const char *key, int16_t default_val)
{
    int16_t val = default_val;
    esp_err_t err = nvs_get_i16(h, key, &val);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_i16(\"%s\"): %s", key, esp_err_to_name(err));
    }
    return val;
}

int32_t nvs_hal_get_i32(nvs_handle_t h, const char *key, int32_t default_val)
{
    int32_t val = default_val;
    esp_err_t err = nvs_get_i32(h, key, &val);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_i32(\"%s\"): %s", key, esp_err_to_name(err));
    }
    return val;
}

/* ── Escrita ─────────────────────────────────────────────────────────────── */

esp_err_t nvs_hal_set_u8(nvs_handle_t h, const char *key, uint8_t val)
{
    esp_err_t err = nvs_set_u8(h, key, val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u8(\"%s\"): %s", key, esp_err_to_name(err));
    }
    return err;
}

esp_err_t nvs_hal_set_u16(nvs_handle_t h, const char *key, uint16_t val)
{
    esp_err_t err = nvs_set_u16(h, key, val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u16(\"%s\"): %s", key, esp_err_to_name(err));
    }
    return err;
}

esp_err_t nvs_hal_set_u32(nvs_handle_t h, const char *key, uint32_t val)
{
    esp_err_t err = nvs_set_u32(h, key, val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u32(\"%s\"): %s", key, esp_err_to_name(err));
    }
    return err;
}

esp_err_t nvs_hal_set_i8(nvs_handle_t h, const char *key, int8_t val)
{
    esp_err_t err = nvs_set_i8(h, key, val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_i8(\"%s\"): %s", key, esp_err_to_name(err));
    }
    return err;
}

esp_err_t nvs_hal_set_i16(nvs_handle_t h, const char *key, int16_t val)
{
    esp_err_t err = nvs_set_i16(h, key, val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_i16(\"%s\"): %s", key, esp_err_to_name(err));
    }
    return err;
}

esp_err_t nvs_hal_set_i32(nvs_handle_t h, const char *key, int32_t val)
{
    esp_err_t err = nvs_set_i32(h, key, val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_i32(\"%s\"): %s", key, esp_err_to_name(err));
    }
    return err;
}
