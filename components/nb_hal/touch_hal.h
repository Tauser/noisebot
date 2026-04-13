/*
 * touch_hal.h — HAL para touch capacitivo ESP32-S3 (Layer 1)
 *
 * Wrapper sobre o peripheral touch do ESP-IDF.
 * Expõe: init, read, baseline.
 *
 * Hardware: GPIO 2 = TOUCH_PAD_NUM1, fita de cobre.
 *
 * Direção de leitura: valor AUMENTA quando tocado (ESP32-S3 touch v2).
 * Threshold = baseline × (1 + sensitivity_factor).
 *
 * Calibração: realizada em nb_touch_hal_init() — 10 amostras em 100ms.
 * Sistema não deve ser tocado durante o boot.
 */

#ifndef NB_TOUCH_HAL_H
#define NB_TOUCH_HAL_H

#include "esp_err.h"
#include <stdint.h>

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o periférico touch e calibra o baseline.
 *
 * Demora ~300ms (200ms de settle + 100ms de calibração).
 * Não tocar o sensor durante este período.
 *
 * @return ESP_OK em sucesso.
 */
esp_err_t nb_touch_hal_init(void);

/**
 * @brief Lê o valor bruto atual do sensor.
 *
 * @param[out] raw  Valor bruto (não filtrado). Obrigatório.
 * @return ESP_OK em sucesso.
 */
esp_err_t nb_touch_hal_read(uint32_t *raw);

/**
 * @brief Retorna o baseline calculado na calibração de boot.
 *
 * @return Valor de baseline (0 se não calibrado).
 */
uint32_t nb_touch_hal_get_baseline(void);

/**
 * @brief Libera recursos e desinicializa o periférico.
 */
void nb_touch_hal_deinit(void);

#endif /* NB_TOUCH_HAL_H */
