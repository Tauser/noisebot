/*
 * render_service.h — Serviço de renderização do NoiseBot
 *
 * Layer 4 — Services. Depende do nb_hal/display_hal (Layer 1).
 *
 * Responsabilidades:
 *   - Manter dois sprite 240×320 em PSRAM (double buffer).
 *   - Executar render_task a 30fps (Core 1, prioridade 4).
 *   - Despachar callbacks de layer em ordem de z_order por frame.
 *   - Logar métricas (fps, tempo por layer, tempo de push SPI) via ESP_LOGD.
 *
 * Uso típico:
 *   render_service_init();
 *   render_service_register_layer(10, my_bg_layer, NULL);
 *   render_service_register_layer(20, my_face_layer, &face_ctx);
 *   render_service_start();
 *
 * Task:
 *   Nome:       "nb_render_task"
 *   Stack:      4096 bytes
 *   Prioridade: 4
 *   Core:       1 (app core)
 */

#ifndef NB_RENDER_SERVICE_H
#define NB_RENDER_SERVICE_H

#include "esp_err.h"
#include "display_hal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Máximo de layers registrados simultaneamente. */
#define NB_RENDER_MAX_LAYERS    8

/**
 * @brief Callback de desenho de uma layer.
 *
 * Chamado a partir da render_task a cada frame, na ordem crescente de z_order.
 * O canvas é um LGFX_Sprite* (PSRAM, 16bpp, RGB565) que representa o back buffer.
 *
 * C++ callers: cast canvas para LGFX_Sprite* para usar a API completa de desenho.
 * C callers: pode usar display_hal_sprite_* mas esses são limitados (push only).
 *
 * @param canvas  Back buffer para desenhar. Nunca NULL.
 * @param ctx     Contexto do usuário passado em render_service_register_layer().
 */
typedef void (*nb_render_layer_fn_t)(nb_display_sprite_t canvas, void *ctx);

/* ── Ciclo de vida ───────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o render_service.
 *
 * Aloca dois sprites 240×320 em PSRAM (double buffer).
 * Deve ser chamado após display_hal_init() e antes de render_service_start().
 *
 * @return ESP_OK            Sprites alocados com sucesso.
 * @return ESP_ERR_NO_MEM   PSRAM insuficiente para os dois sprites.
 * @return ESP_ERR_INVALID_STATE  Já inicializado.
 */
esp_err_t render_service_init(void);

/**
 * @brief Inicia a render_task.
 *
 * @return ESP_OK ou ESP_FAIL se a task não pôde ser criada.
 */
esp_err_t render_service_start(void);

/**
 * @brief Para a render_task e aguarda encerramento.
 */
void render_service_stop(void);

/* ── Sistema de layers ───────────────────────────────────────────────────── */

/**
 * @brief Registra uma layer de desenho.
 *
 * Layers com mesmo z_order são entregues na ordem de registro.
 * Thread-safe: pode ser chamado de qualquer task.
 *
 * @param z_order  Ordem de pintura (menor = fundo).
 * @param fn       Callback de desenho (não NULL).
 * @param ctx      Contexto opaco passado ao callback.
 * @return ESP_OK               Layer registrada.
 * @return ESP_ERR_NO_MEM       Tabela de layers cheia (max NB_RENDER_MAX_LAYERS).
 * @return ESP_ERR_INVALID_ARG  fn é NULL.
 */
esp_err_t render_service_register_layer(uint8_t z_order,
                                        nb_render_layer_fn_t fn,
                                        void *ctx);

/**
 * @brief Remove uma layer pelo ponteiro de função.
 *
 * No-op silencioso se fn não estiver registrada.
 * Thread-safe.
 *
 * @param fn  Callback registrado anteriormente.
 */
void render_service_unregister_layer(nb_render_layer_fn_t fn);

#ifdef __cplusplus
}
#endif

#endif /* NB_RENDER_SERVICE_H */
