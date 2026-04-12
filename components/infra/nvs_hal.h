/*
 * nvs_hal.h — Wrapper type-safe sobre a API NVS do ESP-IDF
 *
 * Simplifica o acesso ao NVS em dois aspectos:
 *
 *   1. get-with-default: retorna o valor armazenado ou um default se a chave
 *      não existir, sem exigir tratamento de ESP_ERR_NVS_NOT_FOUND no call site.
 *
 *   2. Wrappers de open/close/commit com log de erro integrado.
 *
 * Não inicializa NVS flash — isso é responsabilidade do boot_manager
 * (nvs_flash_init em PHASE_EARLY). nvs_hal só gerencia handles e I/O.
 *
 * Thread safety: o ESP-IDF garante thread safety internamente para handles
 * distintos. Não compartilhar handles entre tasks sem mutex externo.
 */

#ifndef NB_NVS_HAL_H
#define NB_NVS_HAL_H

#include "nvs.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* ── Open / Close / Commit ───────────────────────────────────────────────── */

/**
 * @brief Abre um namespace NVS.
 *
 * Wrapper sobre nvs_open() com log de erro integrado.
 *
 * @param ns         Nome do namespace (máx 15 chars).
 * @param mode       NVS_READONLY ou NVS_READWRITE.
 * @param out_handle Handle resultante.
 * @return ESP_OK ou erro de nvs_open().
 */
esp_err_t nvs_hal_open(const char *ns, nvs_open_mode_t mode,
                       nvs_handle_t *out_handle);

/**
 * @brief Fecha um handle NVS.
 */
void nvs_hal_close(nvs_handle_t handle);

/**
 * @brief Persiste escritas pendentes no handle.
 *
 * @return ESP_OK ou erro de nvs_commit().
 */
esp_err_t nvs_hal_commit(nvs_handle_t handle);

/**
 * @brief Verifica se uma chave existe no handle.
 *
 * @return true se a chave existe e tem tipo nvs_type != NVS_TYPE_ANY.
 */
bool nvs_hal_key_exists(nvs_handle_t handle, const char *key);

/* ── Leitura com default ─────────────────────────────────────────────────── */
/*
 * Retorna o valor armazenado, ou default_val se:
 *   - A chave não existe (ESP_ERR_NVS_NOT_FOUND)
 *   - Qualquer outro erro de leitura (loga warning)
 */

uint8_t  nvs_hal_get_u8 (nvs_handle_t h, const char *key, uint8_t  default_val);
uint16_t nvs_hal_get_u16(nvs_handle_t h, const char *key, uint16_t default_val);
uint32_t nvs_hal_get_u32(nvs_handle_t h, const char *key, uint32_t default_val);
int8_t   nvs_hal_get_i8 (nvs_handle_t h, const char *key, int8_t   default_val);
int16_t  nvs_hal_get_i16(nvs_handle_t h, const char *key, int16_t  default_val);
int32_t  nvs_hal_get_i32(nvs_handle_t h, const char *key, int32_t  default_val);

/* ── Escrita ─────────────────────────────────────────────────────────────── */
/*
 * Nota: não fazem commit automaticamente. Chamar nvs_hal_commit() após
 * um lote de escritas para minimizar ciclos de escrita na flash.
 */

esp_err_t nvs_hal_set_u8 (nvs_handle_t h, const char *key, uint8_t  val);
esp_err_t nvs_hal_set_u16(nvs_handle_t h, const char *key, uint16_t val);
esp_err_t nvs_hal_set_u32(nvs_handle_t h, const char *key, uint32_t val);
esp_err_t nvs_hal_set_i8 (nvs_handle_t h, const char *key, int8_t   val);
esp_err_t nvs_hal_set_i16(nvs_handle_t h, const char *key, int16_t  val);
esp_err_t nvs_hal_set_i32(nvs_handle_t h, const char *key, int32_t  val);

#endif /* NB_NVS_HAL_H */
