/*
 * led_hal.h — HAL para WS2812 via RMT (Layer 1)
 *
 * Wrapper fino sobre o driver led_strip do ESP-IDF.
 * Expõe API C pura: init, set_pixel, flush, clear.
 *
 * Não contém lógica de animação nem de prioridade —
 * isso é responsabilidade do led_service (Layer 4).
 *
 * Hardware: GPIO 19, 2 LEDs WS2812 em série (NB_LED_PIN_DATA, NB_LED_COUNT).
 */

#ifndef NB_LED_HAL_H
#define NB_LED_HAL_H

#include "esp_err.h"
#include <stdint.h>

/* ── Tipos ───────────────────────────────────────────────────────────────── */

/** Cor RGB linear (pré-gamma). */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} nb_led_color_t;

/* Cor preta (conveniência). */
#define NB_LED_COLOR_BLACK  ((nb_led_color_t){0,   0,   0  })
#define NB_LED_COLOR_WHITE  ((nb_led_color_t){255, 255, 255})
#define NB_LED_COLOR_RED    ((nb_led_color_t){255, 0,   0  })
#define NB_LED_COLOR_GREEN  ((nb_led_color_t){0,   255, 0  })
#define NB_LED_COLOR_BLUE   ((nb_led_color_t){0,   0,   255})

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o driver RMT e o canal led_strip.
 *
 * Deve ser chamado uma vez durante PHASE_HAL do boot_manager.
 * Após init, todos os LEDs são apagados.
 *
 * @return ESP_OK em sucesso.
 */
esp_err_t led_hal_init(void);

/**
 * @brief Define a cor de um LED individual no buffer interno.
 *
 * Não envia ao hardware — chamar led_hal_flush() para efetivar.
 *
 * @param idx   Índice do LED (0 a NB_LED_COUNT-1).
 * @param color Cor desejada (RGB linear, pré-gamma).
 * @return ESP_OK, ESP_ERR_INVALID_ARG se idx fora de range.
 */
esp_err_t led_hal_set_pixel(uint8_t idx, nb_led_color_t color);

/**
 * @brief Envia o buffer atual para os LEDs via RMT.
 *
 * Chamada bloqueante breve (~400µs para 2 LEDs a 10MHz RMT).
 * Chamar somente quando houver mudança real (dirty detection no led_service).
 *
 * @return ESP_OK em sucesso.
 */
esp_err_t led_hal_flush(void);

/**
 * @brief Apaga todos os LEDs (seta preto e faz flush).
 *
 * @return ESP_OK em sucesso.
 */
esp_err_t led_hal_clear(void);

/**
 * @brief Libera recursos do driver led_strip.
 */
void led_hal_deinit(void);

#endif /* NB_LED_HAL_H */
