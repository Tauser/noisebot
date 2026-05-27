/*
 * render_service.h — Serviço de renderização do NoiseBot
 *
 * Layer 4 — Services. Depende do nb_hal/display_hal (Layer 1).
 *
 * Responsabilidades:
 *   - Manter dois sprites em PSRAM (double buffer).
 *   - Pipeline dual-task: render_task desenha, push_task envia via SPI DMA.
 *   - Despachar callbacks de layer em ordem de z_order por frame.
 *   - Enviar ao display apenas a região suja (dirty rect) declarada pelas layers.
 *   - Logar métricas (fps, clear, layer, push, dirty) via ESP_LOGI a cada 5s.
 *
 * Modelo de dirty rect:
 *   Cada layer callback deve chamar render_service_mark_dirty() para as regiões
 *   que modificou. O render_service acumula um bounding box global por frame e
 *   envia apenas essa região. Layers que não chamam mark_dirty acionam push
 *   completo (fallback seguro e compatível com código legado).
 *   Frames sem nenhum dirty são pulados (sem push SPI).
 *
 * Uso típico:
 *   render_service_init();
 *   render_service_register_layer(10, my_bg_layer, NULL);
 *   render_service_register_layer(20, my_face_layer, &face_ctx);
 *   render_service_start();
 *
 *   // Dentro de my_face_layer():
 *   canvas->fillRect(eye_x, eye_y, eye_w, eye_h, color);
 *   render_service_mark_dirty(eye_x, eye_y, eye_w, eye_h);
 *
 * Tasks:
 *   nb_render_task: Core 1, prioridade 7, stack 4096 bytes
 *   nb_push_task:   Core 1, prioridade 7, stack 3072 bytes
 */

#ifndef NB_RENDER_SERVICE_H
#define NB_RENDER_SERVICE_H

#include "esp_err.h"
#include "display_hal.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Máximo de layers registrados simultaneamente. */
#define NB_RENDER_MAX_LAYERS    8

/**
 * @brief Callback de desenho de uma layer.
 *
 * Chamado pela render_task a cada frame, na ordem crescente de z_order.
 * O canvas é um LGFX_Sprite* (PSRAM, 16bpp, RGB565) do back buffer.
 *
 * Após desenhar, chamar render_service_mark_dirty() para cada região modificada.
 * Não chamar mark_dirty resulta em push completo do frame (fallback seguro).
 *
 * @param canvas  Back buffer. Nunca NULL.
 * @param ctx     Contexto do usuário passado em render_service_register_layer().
 */
typedef void (*nb_render_layer_fn_t)(nb_display_sprite_t canvas, void *ctx);

/* ── Ciclo de vida ───────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o render_service.
 *
 * Aloca dois sprites (NB_DISP_WIDTH × NB_DISP_HEIGHT) em PSRAM.
 * Deve ser chamado após display_hal_init() e antes de render_service_start().
 *
 * @return ESP_OK, ESP_ERR_NO_MEM, ou ESP_ERR_INVALID_STATE.
 */
esp_err_t render_service_init(void);

/**
 * @brief Inicia render_task e push_task.
 *
 * @return ESP_OK ou ESP_FAIL.
 */
esp_err_t render_service_start(void);

/**
 * @brief Para ambas as tasks e aguarda encerramento.
 */
void render_service_stop(void);

/* ── Sistema de layers ───────────────────────────────────────────────────── */

/**
 * @brief Registra uma layer de desenho.
 *
 * Thread-safe. Layers com mesmo z_order são entregues na ordem de registro.
 *
 * @param z_order  Ordem de pintura (menor = fundo).
 * @param fn       Callback de desenho (não NULL).
 * @param ctx      Contexto opaco passado ao callback.
 * @return ESP_OK, ESP_ERR_NO_MEM, ou ESP_ERR_INVALID_ARG.
 */
esp_err_t render_service_register_layer(uint8_t z_order,
                                        nb_render_layer_fn_t fn,
                                        void *ctx);

/**
 * @brief Remove uma layer pelo ponteiro de função.
 *
 * No-op silencioso se fn não estiver registrada. Thread-safe.
 */
void render_service_unregister_layer(nb_render_layer_fn_t fn);

/* ── API de dirty rect ───────────────────────────────────────────────────── */

/**
 * @brief Declara uma região retangular como modificada neste frame.
 *
 * Chamar dentro de um nb_render_layer_fn_t callback, após cada operação de
 * desenho. O render_service acumula um bounding box global e envia ao display
 * apenas essa região, reduzindo o tempo de push proporcionalmente ao conteúdo
 * que realmente mudou.
 *
 * Regiões fora dos limites do display são clampadas silenciosamente.
 * Thread-safety: chamar APENAS de dentro de um layer callback (render_task).
 *
 * @param x  Coluna superior-esquerda da região modificada.
 * @param y  Linha superior-esquerda da região modificada.
 * @param w  Largura em pixels.
 * @param h  Altura em pixels.
 */
void render_service_mark_dirty(int x, int y, int w, int h);

/**
 * @brief Força push completo no próximo frame.
 *
 * Usar após mudanças de cena, init de conteúdo estático, ou sempre que o
 * histórico de dirty rect for insuficiente para garantir consistência visual.
 * Thread-safe: pode ser chamado de qualquer task.
 */
void render_service_force_full_refresh(void);

/**
 * @brief Ajusta o brilho visual da tela (0-255).
 *
 * Primeiro tenta aplicar no backlight físico via display HAL. Em painéis sem
 * pino BL, o render_service aplica dimming por software no frame antes do push.
 * Thread-safe: pode ser chamado de qualquer task.
 *
 * @param level 0 = escuro, 255 = brilho máximo.
 */
void render_service_set_brightness(uint8_t level);

/* ── Métricas ────────────────────────────────────────────────────────────── */

/**
 * @brief Retorna o último FPS medido. Atualizado a cada 5s. Zero antes do
 *        primeiro intervalo completo.
 */
float render_service_get_fps(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_RENDER_SERVICE_H */
