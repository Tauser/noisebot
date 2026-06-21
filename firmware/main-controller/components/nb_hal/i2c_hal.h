/*
 * i2c_hal.h — HAL de barramento I2C compartilhado (Layer 1)
 *
 * Inicializa o barramento I2C master uma única vez (GPIO 4/5).
 * Todos os HALs de sensor (imu, env, prox, battery, camera SCCB)
 * obtêm o handle via nb_i2c_hal_get_bus() — nenhum inicializa I2C próprio.
 *
 * Endereços esperados no barramento:
 *   0x3C  OV2640 (SCCB)
 *   0x39  APDS-9960 proximidade
 *   0x44  SHT40 temperatura/humidade
 *   0x68  MPU-6050 IMU
 *   0x6B  bq25185 charger
 *   0x36  MAX17048 fuel gauge
 *
 * Sensores com pino INT (IMU, APDS-9960) usam polling via schedule_service —
 * não há GPIO livre para interrupção (GPIO 3 = WS2812 RMT, não disponível).
 */

#ifndef NB_I2C_HAL_H
#define NB_I2C_HAL_H

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "nb_hw_config_profile.h"
#include <stdint.h>
#include <stdbool.h>

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o barramento I2C master.
 *
 * Deve ser chamado uma única vez no boot, antes de qualquer HAL de sensor.
 * Configura GPIO 4/5 com pull-ups internos e clock 400kHz.
 *
 * @return ESP_OK em sucesso, ESP_ERR_INVALID_STATE se já inicializado.
 */
esp_err_t nb_i2c_hal_init(void);

/**
 * @brief Retorna o handle do barramento I2C para uso pelos HALs de sensor.
 *
 * @return Handle válido após nb_i2c_hal_init(). NULL se não inicializado.
 */
i2c_master_bus_handle_t nb_i2c_hal_get_bus(void);

/**
 * @brief Verifica se o barramento foi inicializado.
 */
bool nb_i2c_hal_is_ready(void);

/**
 * @brief Escaneia o barramento e loga os endereços que respondem.
 *
 * Uso em debug e diagnósticos de boot. Não chamar em runtime crítico.
 * Testa endereços 0x08–0x77 com timeout de 10ms por slot.
 *
 * @param[out] found     Buffer para endereços encontrados. Pode ser NULL.
 * @param[in]  max_found Tamanho do buffer. Ignorado se found == NULL.
 * @param[out] count     Número de dispositivos encontrados. Pode ser NULL.
 * @return ESP_OK mesmo se nenhum dispositivo for encontrado.
 */
esp_err_t nb_i2c_hal_scan(uint8_t *found, size_t max_found, size_t *count);

/**
 * @brief Desinicializa o barramento I2C e libera recursos.
 *
 * Usado em power-off controlado ou safe mode.
 * Após esta chamada, nb_i2c_hal_get_bus() retorna NULL.
 */
void nb_i2c_hal_deinit(void);

#endif /* NB_I2C_HAL_H */
