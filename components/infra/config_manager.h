#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Namespaces NVS — um por modulo                                    */
/* ------------------------------------------------------------------ */

#define CFG_NS_SYSTEM    "nb_system"
#define CFG_NS_POWER     "nb_power"
#define CFG_NS_SERVO     "nb_servo"
#define CFG_NS_TOUCH     "nb_touch"
#define CFG_NS_AUDIO     "nb_audio"
#define CFG_NS_DISPLAY   "nb_display"
#define CFG_NS_IDENTITY  "nb_identity"
#define CFG_NS_FEATURES  "nb_features"
#define CFG_NS_IMU       "nb_imu"
#define CFG_NS_MEMORY    "nb_memory"

/* Versao atual do schema de NVS — incrementar ao mudar chaves */
#define CFG_SCHEMA_VERSION  1

/* ------------------------------------------------------------------ */
/*  API                                                                */
/* ------------------------------------------------------------------ */

/*
 * config_manager_init() — Inicializa NVS e verifica/migra schema.
 * Chamado no BOOT_PHASE_0, logo apos nvs_flash_init().
 */
esp_err_t config_manager_init(void);

/* Leitura com fallback para valor default se chave ausente */
esp_err_t config_get_u8 (const char *ns, const char *key, uint8_t  *val, uint8_t  def);
esp_err_t config_get_u16(const char *ns, const char *key, uint16_t *val, uint16_t def);
esp_err_t config_get_u32(const char *ns, const char *key, uint32_t *val, uint32_t def);
esp_err_t config_get_str(const char *ns, const char *key, char *buf, size_t len, const char *def);
esp_err_t config_get_i8 (const char *ns, const char *key, int8_t   *val, int8_t   def);
esp_err_t config_get_i16(const char *ns, const char *key, int16_t  *val, int16_t  def);

/* Escrita — sempre via esta API, nunca diretamente em NVS */
esp_err_t config_set_u8 (const char *ns, const char *key, uint8_t  val);
esp_err_t config_set_u16(const char *ns, const char *key, uint16_t val);
esp_err_t config_set_u32(const char *ns, const char *key, uint32_t val);
esp_err_t config_set_str(const char *ns, const char *key, const char *val);
esp_err_t config_set_i8 (const char *ns, const char *key, int8_t   val);
esp_err_t config_set_i16(const char *ns, const char *key, int16_t  val);

/* Utilitarios */
uint8_t   config_get_schema_version(void);
esp_err_t config_erase_namespace(const char *ns);
esp_err_t config_erase_all(void);  /* Reset de fabrica */

#ifdef __cplusplus
}
#endif
