#include "config_manager.h"
#include "logger.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "config_mgr";

/* ------------------------------------------------------------------ */

static esp_err_t prv_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h) {
    return nvs_open(ns, mode, h);
}

esp_err_t config_manager_init(void) {
    uint8_t ver = 0;
    esp_err_t ret = config_get_u8(CFG_NS_SYSTEM, "schema_ver", &ver, 0);
    if (ret == ESP_OK && ver == 0) {
        /* Primeiro boot: gravar versao */
        config_set_u8(CFG_NS_SYSTEM, "schema_ver", CFG_SCHEMA_VERSION);
        NB_LOGI(TAG, "NVS inicializada (schema v%d)", CFG_SCHEMA_VERSION);
    } else if (ver > 0 && ver != CFG_SCHEMA_VERSION) {
        NB_LOGW(TAG, "Schema NVS v%d != firmware v%d — migracao necessaria",
                ver, CFG_SCHEMA_VERSION);
        /* TODO: chamar funcao de migracao quando schema mudar */
    }
    NB_LOGI(TAG, "ConfigManager OK (schema v%d)", CFG_SCHEMA_VERSION);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Leitura                                                            */
/* ------------------------------------------------------------------ */

esp_err_t config_get_u8(const char *ns, const char *key, uint8_t *val, uint8_t def) {
    nvs_handle_t h;
    if (prv_open(ns, NVS_READONLY, &h) != ESP_OK) { *val = def; return ESP_OK; }
    esp_err_t ret = nvs_get_u8(h, key, val);
    if (ret != ESP_OK) *val = def;
    nvs_close(h);
    return ESP_OK;
}

esp_err_t config_get_u16(const char *ns, const char *key, uint16_t *val, uint16_t def) {
    nvs_handle_t h;
    if (prv_open(ns, NVS_READONLY, &h) != ESP_OK) { *val = def; return ESP_OK; }
    esp_err_t ret = nvs_get_u16(h, key, val);
    if (ret != ESP_OK) *val = def;
    nvs_close(h);
    return ESP_OK;
}

esp_err_t config_get_u32(const char *ns, const char *key, uint32_t *val, uint32_t def) {
    nvs_handle_t h;
    if (prv_open(ns, NVS_READONLY, &h) != ESP_OK) { *val = def; return ESP_OK; }
    esp_err_t ret = nvs_get_u32(h, key, val);
    if (ret != ESP_OK) *val = def;
    nvs_close(h);
    return ESP_OK;
}

esp_err_t config_get_i8(const char *ns, const char *key, int8_t *val, int8_t def) {
    nvs_handle_t h;
    if (prv_open(ns, NVS_READONLY, &h) != ESP_OK) { *val = def; return ESP_OK; }
    esp_err_t ret = nvs_get_i8(h, key, val);
    if (ret != ESP_OK) *val = def;
    nvs_close(h);
    return ESP_OK;
}

esp_err_t config_get_i16(const char *ns, const char *key, int16_t *val, int16_t def) {
    nvs_handle_t h;
    if (prv_open(ns, NVS_READONLY, &h) != ESP_OK) { *val = def; return ESP_OK; }
    esp_err_t ret = nvs_get_i16(h, key, val);
    if (ret != ESP_OK) *val = def;
    nvs_close(h);
    return ESP_OK;
}

esp_err_t config_get_str(const char *ns, const char *key, char *buf, size_t len, const char *def) {
    nvs_handle_t h;
    if (prv_open(ns, NVS_READONLY, &h) != ESP_OK) goto use_def;
    size_t needed = len;
    esp_err_t ret = nvs_get_str(h, key, buf, &needed);
    nvs_close(h);
    if (ret == ESP_OK) return ESP_OK;
use_def:
    if (def) strncpy(buf, def, len - 1);
    else     buf[0] = '\0';
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Escrita                                                            */
/* ------------------------------------------------------------------ */

esp_err_t config_set_u8(const char *ns, const char *key, uint8_t val) {
    nvs_handle_t h;
    esp_err_t ret = prv_open(ns, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    nvs_set_u8(h, key, val);
    ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

esp_err_t config_set_u16(const char *ns, const char *key, uint16_t val) {
    nvs_handle_t h;
    esp_err_t ret = prv_open(ns, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    nvs_set_u16(h, key, val);
    ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

esp_err_t config_set_u32(const char *ns, const char *key, uint32_t val) {
    nvs_handle_t h;
    esp_err_t ret = prv_open(ns, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    nvs_set_u32(h, key, val);
    ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

esp_err_t config_set_i8(const char *ns, const char *key, int8_t val) {
    nvs_handle_t h;
    esp_err_t ret = prv_open(ns, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    nvs_set_i8(h, key, val);
    ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

esp_err_t config_set_i16(const char *ns, const char *key, int16_t val) {
    nvs_handle_t h;
    esp_err_t ret = prv_open(ns, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    nvs_set_i16(h, key, val);
    ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

esp_err_t config_set_str(const char *ns, const char *key, const char *val) {
    nvs_handle_t h;
    esp_err_t ret = prv_open(ns, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    nvs_set_str(h, key, val);
    ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

/* ------------------------------------------------------------------ */

uint8_t config_get_schema_version(void) {
    uint8_t ver = 0;
    config_get_u8(CFG_NS_SYSTEM, "schema_ver", &ver, 0);
    return ver;
}

esp_err_t config_erase_namespace(const char *ns) {
    nvs_handle_t h;
    esp_err_t ret = prv_open(ns, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    ret = nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    return ret;
}

esp_err_t config_erase_all(void) {
    NB_LOGW(TAG, "ERASE ALL — reset de fabrica");
    return nvs_flash_erase();
}
