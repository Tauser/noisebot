/*
 * display_hal.h — HAL do display do NoiseBot
 *
 * API C pura sobre LovyanGFX. Suporta ST7789 240×240 e ILI9342 320×240.
 * Driver ativo selecionado em tempo de compilação via NB_DISP_DRIVER_ILI9342.
 *
 * Regra inegociável: TODOS os sprites são alocados em PSRAM.
 * Nunca usar SRAM para framebuffer ou sprite de display.
 *
 * Uso típico pelo render_service:
 *   1. display_hal_init()
 *   2. sprite = display_hal_sprite_create(240, 240)
 *   3. loop: desenhar no sprite via display_hal_get_lgfx() | pushSprite()
 *   4. display_hal_sprite_push(sprite, 0, 0)
 */

#ifndef NB_DISPLAY_HAL_H
#define NB_DISPLAY_HAL_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Handle opaco para sprite off-screen (LGFX_Sprite alocado em PSRAM). */
typedef void *nb_display_sprite_t;

/* ── Inicialização ───────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o display.
 *
 * Configura SPI2, inicializa o painel (ST7789 ou ILI9342), limpa a tela a preto.
 * Deve ser chamado em PHASE_HAL do boot_manager.
 *
 * @return ESP_OK ou ESP_FAIL se já inicializado.
 */
esp_err_t display_hal_init(void);

/* ── Controle de tela ────────────────────────────────────────────────────── */

/**
 * @brief Define o brilho do backlight (0–255).
 *
 * No-op se o driver ativo não tem pino de backlight (ST7789 sem BL).
 * Para ILI9342 com BL via LEDC: controle PWM suave.
 *
 * @param level 0 = apagado, 255 = brilho máximo.
 */
void display_hal_set_brightness(uint8_t level);

/**
 * @brief Preenche toda a tela com uma cor RGB565.
 */
void display_hal_fill(uint16_t color_rgb565);

/**
 * @brief Largura em pixels do display ativo.
 */
int display_hal_width(void);

/**
 * @brief Altura em pixels do display ativo.
 */
int display_hal_height(void);

/* ── Sprites (off-screen buffers em PSRAM) ───────────────────────────────── */

/**
 * @brief Cria sprite off-screen alocado em PSRAM (16 bpp, RGB565).
 *
 * Falha se PSRAM insuficiente. Retorna NULL — caller deve verificar.
 *
 * @param w Largura em pixels.
 * @param h Altura em pixels.
 * @return Handle opaco, ou NULL em falha.
 */
nb_display_sprite_t display_hal_sprite_create(int w, int h);

/**
 * @brief Envia sprite para o display na posição (x, y).
 *
 * Transferência via DMA SPI. Retorna após conclusão.
 *
 * @param sprite Handle retornado por display_hal_sprite_create(). Ignorado se NULL.
 * @param x      Coluna do canto superior esquerdo no display.
 * @param y      Linha do canto superior esquerdo no display.
 */
void display_hal_sprite_push(nb_display_sprite_t sprite, int x, int y);

/**
 * @brief Libera o sprite da PSRAM.
 *
 * Após retorno, o handle é inválido. Ignorado se NULL.
 */
void display_hal_sprite_delete(nb_display_sprite_t sprite);

/* ── Acesso ao objeto LGFX (C++ only) ───────────────────────────────────── */

/**
 * @brief Retorna ponteiro para o objeto NB_LGFX subjacente.
 *
 * Permite que render_service (C++) use a API completa do LovyanGFX para
 * desenhar no dispositivo ou obter um LGFX_Sprite vinculado ao display.
 * Cast para `NB_LGFX*` (inclua display_lgfx_config.hpp para ter o tipo).
 *
 * Disponível apenas em C++.
 */
#ifdef __cplusplus
void *display_hal_get_lgfx(void);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NB_DISPLAY_HAL_H */
