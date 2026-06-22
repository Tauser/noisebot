#ifndef NB_DISPLAY_PROTOCOL_H
#define NB_DISPLAY_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_DISPLAY_COMMAND_VERSION 1U
#define NB_DISPLAY_EXPRESSION_COUNT 10U
#define NB_DISPLAY_GAZE_MIN       (-1000)
#define NB_DISPLAY_GAZE_MAX       1000

#define NB_DISPLAY_OVERLAY_LISTENING (1U << 0)
#define NB_DISPLAY_OVERLAY_SPEAKING  (1U << 1)
#define NB_DISPLAY_OVERLAY_SLEEPING  (1U << 2)
#define NB_DISPLAY_OVERLAY_ALERT     (1U << 3)
#define NB_DISPLAY_OVERLAY_HEART     (1U << 4)
#define NB_DISPLAY_OVERLAY_BLUSH     (1U << 5)
#define NB_DISPLAY_OVERLAY_MESSAGE   (1U << 6)
#define NB_DISPLAY_OVERLAY_TIMER     (1U << 7)
#define NB_DISPLAY_OVERLAY_ALL       0x00ffU

typedef enum {
    NB_DISPLAY_OP_SET_SCENE = 1,
    NB_DISPLAY_OP_FORCE_REFRESH,
    NB_DISPLAY_OP_SET_POWER,
} nb_display_opcode_t;

typedef enum {
    NB_DISPLAY_POWER_OFF = 0,
    NB_DISPLAY_POWER_ON,
} nb_display_power_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t opcode;
    uint8_t expression;
    uint8_t brightness;
    int16_t gaze_x_milli;
    int16_t gaze_y_milli;
    uint16_t overlay_flags;
    uint16_t reserved;
    uint32_t generation;
} nb_display_command_t;

_Static_assert(sizeof(nb_display_command_t) == 16U,
               "display command wire size changed");

bool nb_display_command_is_valid(const nb_display_command_t *command,
                                 size_t length);
bool nb_display_generation_is_newer(uint32_t candidate, uint32_t current);

/*
 * Contrato v2 (DM2.7) — mensagens modulares em vez da struct monolítica v1.
 * v1 (nb_display_command_t) continua válido e em uso: o head só recebe v2
 * se anunciar NB_LINK_CAP_DISPLAY_V2 no HELLO; sem essa capability, o main
 * cai para NB_LINK_MSG_DISPLAY_COMMAND (v1) sem nenhuma mudança de
 * comportamento visual.
 *
 * block_mask em nb_display_scene_v2_t reserva espaço para blocos futuros
 * (animação/estado, DM2.8-DM2.9) sem quebrar o wire format; hoje só
 * NB_DISPLAY_V2_BLOCK_BASE existe — os mesmos campos do v1 (expressão,
 * gaze, brilho, overlays), idempotentes por generation.
 */

#define NB_DISPLAY_SCENE_V2_VERSION     2U
#define NB_DISPLAY_TRANSIENT_V2_VERSION 2U
#define NB_DISPLAY_RESULT_V2_VERSION    2U

typedef enum {
    NB_DISPLAY_V2_BLOCK_BASE = 1U << 0,
} nb_display_v2_block_t;

#define NB_DISPLAY_V2_BLOCK_ALL NB_DISPLAY_V2_BLOCK_BASE

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t block_mask;
    uint8_t expression;
    uint8_t brightness;
    int16_t gaze_x_milli;
    int16_t gaze_y_milli;
    uint16_t overlay_flags;
    uint16_t reserved;
    uint32_t generation;
} nb_display_scene_v2_t;

_Static_assert(sizeof(nb_display_scene_v2_t) == 16U,
               "display scene v2 wire size changed");

/*
 * Comando transiente (glance, e futuramente blink/tilt em DM2.9): tem
 * identidade própria (transient_id), deadline monotônico local ao head e
 * política de cancelamento explícita — nunca sobrescreve silenciosamente
 * outro transiente em voo, o receptor decide por ID.
 */
typedef enum {
    NB_DISPLAY_TRANSIENT_OP_START  = 1,
    NB_DISPLAY_TRANSIENT_OP_CANCEL = 2,
} nb_display_transient_op_t;

typedef enum {
    NB_DISPLAY_TRANSIENT_KIND_GLANCE = 1,
} nb_display_transient_kind_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t op;
    uint8_t kind;
    uint8_t reserved8;
    uint32_t transient_id;
    int16_t gaze_x_milli;
    int16_t gaze_y_milli;
    uint32_t deadline_ms;
} nb_display_transient_v2_t;

_Static_assert(sizeof(nb_display_transient_v2_t) == 16U,
               "display transient v2 wire size changed");

/*
 * Telemetria de aplicação (head → main, canal EVENT): confirma se a SCENE
 * ou o TRANSIENT foram de fato aplicados, rejeitados, expiraram ou foram
 * cancelados — distinto do ACK de transporte (NB_LINK_MSG_ACK), que só
 * confirma que o frame chegou, não que foi aceito pela aplicação.
 */
typedef enum {
    NB_DISPLAY_RESULT_OK = 0,
    NB_DISPLAY_RESULT_REJECTED_VERSION,
    NB_DISPLAY_RESULT_REJECTED_GENERATION_STALE,
    NB_DISPLAY_RESULT_REJECTED_INVALID,
    NB_DISPLAY_RESULT_EXPIRED,
    NB_DISPLAY_RESULT_CANCELLED,
} nb_display_result_status_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t status;
    uint16_t reserved;
    uint32_t transient_id;
    uint32_t generation;
} nb_display_result_v2_t;

_Static_assert(sizeof(nb_display_result_v2_t) == 12U,
               "display result v2 wire size changed");

/*
 * Ícones de status persistentes (DM2.11 -- fatia mínima da Etapa 16.2).
 * Bitmask genérico (até 32 ícones) -- bit N = NB_UI_STATUS_ICON_* (main,
 * ui_overlay_service.h) com ordinal N. O main envia `s_status_icon_flags`
 * direto (sem tradução); por isso os bits aqui têm que casar exatamente
 * com a ordem do enum do main, não com a ordem de chegada no head.
 * Snapshot idempotente (sem generation própria -- estado binário,
 * overwrite é seguro).
 */
#define NB_DISPLAY_STATUS_ICONS_V2_VERSION 2U
/* NB_UI_STATUS_ICON_MIC_BLOCKED = 1 no enum do main (MIC_ACTIVE = 0 vem
 * antes) -- bit 1, não bit 0. */
#define NB_DISPLAY_STATUS_ICON_MIC_BLOCKED (1UL << 1)

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t reserved[3];
    uint32_t icon_bits;
} nb_display_status_icons_v2_t;

_Static_assert(sizeof(nb_display_status_icons_v2_t) == 8U,
               "display status icons v2 wire size changed");

bool nb_display_status_icons_v2_is_valid(
    const nb_display_status_icons_v2_t *icons, size_t length);

bool nb_display_scene_v2_is_valid(const nb_display_scene_v2_t *scene,
                                  size_t length);
bool nb_display_transient_v2_is_valid(const nb_display_transient_v2_t *cmd,
                                      size_t length);
bool nb_display_result_v2_is_valid(const nb_display_result_v2_t *result,
                                   size_t length);
bool nb_display_transient_v2_is_expired(uint32_t deadline_ms,
                                        uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* NB_DISPLAY_PROTOCOL_H */
