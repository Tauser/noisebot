/*
 * sd_hal.h — HAL do microSD do NoiseBot (Layer 1)
 *
 * Gerencia o ciclo de vida do cartão SD via SPI3 + FATFS:
 *   - Inicialização do barramento SPI e montagem FATFS.
 *   - Criação da estrutura de diretórios no primeiro mount.
 *   - Detecção de SD removido (modo degradado).
 *   - Re-mount periódico para hot-plug.
 *
 * O ponto de montagem é NB_SD_MOUNT_POINT ("/sdcard").
 * Após sd_hal_init() bem-sucedido, o VFS do ESP-IDF permite acesso
 * via POSIX padrão (fopen/fclose/mkdir/stat).
 *
 * Pinos: definidos em nb_hw_config.h — verificar contra schematic.
 *
 * Task safety:
 *   sd_hal_init() e sd_hal_deinit() devem ser chamados de uma única task.
 *   sd_hal_is_mounted() é thread-safe (leitura de flag atômica).
 */

#ifndef NB_SD_HAL_H
#define NB_SD_HAL_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/** Intervalo entre tentativas de re-mount quando SD não está disponível. */
#define NB_SD_REMOUNT_INTERVAL_S  30U

esp_err_t sd_hal_init(void);
void      sd_hal_deinit(void);
bool      sd_hal_is_mounted(void);
esp_err_t sd_hal_try_remount(void);
esp_err_t sd_hal_get_free_bytes(uint64_t *out_free_bytes);

#endif /* NB_SD_HAL_H */
