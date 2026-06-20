/*
 * nb_head_i2c_hal.h — HAL de barramento I2C compartilhado do head (Layer 1)
 *
 * Inicializa o barramento I2C master uma única vez (NB_HEAD_PIN_I2C_SDA/SCL).
 * O HAL da câmera (SCCB do OV2640, endereço 0x3C) obtém o handle via
 * nb_head_i2c_hal_get_bus() — não inicializa I2C próprio.
 */

#ifndef NB_HEAD_I2C_HAL_H
#define NB_HEAD_I2C_HAL_H

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nb_head_i2c_hal_init(void);
i2c_master_bus_handle_t nb_head_i2c_hal_get_bus(void);
bool nb_head_i2c_hal_is_ready(void);
esp_err_t nb_head_i2c_hal_scan(uint8_t *found, size_t max_found, size_t *count);
void nb_head_i2c_hal_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_HEAD_I2C_HAL_H */
