#ifndef NB_MAIN_LINK_SERVICE_H
#define NB_MAIN_LINK_SERVICE_H

#include "esp_err.h"
#include "nb_camera_protocol.h"
#include "nb_display_protocol.h"
#include "nb_link_engine.h"

/*
 * Retorna ESP_ERR_NOT_SUPPORTED sem tocar GPIO/SPI enquanto o enlace estiver
 * desabilitado por configuração.
 */
esp_err_t nb_main_link_service_init(void);
nb_link_state_t nb_main_link_service_state(void);
esp_err_t nb_main_link_service_reset_head(void);
esp_err_t nb_main_link_service_queue_display(
    const nb_display_command_t *command);

/*
 * Envia um comando de câmera ao head sob demanda (fire-once, sem fila de
 * estado persistente como o display). Falha com ESP_ERR_INVALID_STATE fora
 * de READY e com ESP_ERR_NOT_SUPPORTED se o head não anunciou
 * NB_LINK_CAP_CAMERA_SEMANTIC.
 */
esp_err_t nb_main_link_service_request_camera(
    const nb_camera_link_command_t *command);

/*
 * Copia o último nb_camera_link_event_t recebido do head. Retorna
 * ESP_ERR_NOT_FOUND se nenhum evento chegou ainda.
 */
esp_err_t nb_main_link_service_get_last_camera_event(
    nb_camera_link_event_t *out_event);

/*
 * Contrato visual v2 (DM2.7). As duas funções de envio só enfileiram —
 * quem chama nb_link_engine_send é exclusivamente a link_task, nunca o
 * chamador. Falham com ESP_ERR_NOT_SUPPORTED se o head não anunciou
 * NB_LINK_CAP_DISPLAY_V2 (peer ainda só fala v1).
 *
 * Scene: snapshot idempotente, fila de profundidade 1 com overwrite —
 * generation mais nova sempre vence, igual ao comportamento do v1.
 *
 * Transient: tem fila própria (profundidade > 1, FIFO). Cheia, retorna
 * ESP_ERR_NO_MEM explicitamente — nunca sobrescreve silenciosamente um
 * transiente em voo.
 */
esp_err_t nb_main_link_service_request_visual_scene_v2(
    const nb_display_scene_v2_t *scene);
esp_err_t nb_main_link_service_request_visual_transient_v2(
    const nb_display_transient_v2_t *command);

/*
 * Copia o último nb_display_result_v2_t recebido do head. Retorna
 * ESP_ERR_NOT_FOUND se nenhum resultado chegou ainda. O slot é único e
 * sobrescrito a cada resultado novo — o chamador deve sempre conferir
 * out_result->transient_id/generation contra o que esperava antes de agir,
 * exatamente para não repetir o bug de correlação do cliente de câmera
 * (docs/DUAL_MCU_MIGRATION_ROADMAP.md, defeitos de DM4.3-4.6).
 */
esp_err_t nb_main_link_service_get_last_visual_result_v2(
    nb_display_result_v2_t *out_result);

/*
 * Ícones de status persistentes (DM2.11, fatia mínima). Snapshot
 * idempotente -- fila de profundidade 1 com overwrite, mesmo padrão da
 * scene v2. Falha com ESP_ERR_NOT_SUPPORTED se o head não anunciou
 * NB_LINK_CAP_DISPLAY_V2.
 */
esp_err_t nb_main_link_service_set_status_icons_v2(uint32_t icon_bits);

#endif /* NB_MAIN_LINK_SERVICE_H */
