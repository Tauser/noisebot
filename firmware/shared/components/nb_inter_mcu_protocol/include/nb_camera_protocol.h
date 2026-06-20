#ifndef NB_CAMERA_PROTOCOL_H
#define NB_CAMERA_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Tipos prefixados nb_camera_link_* (em vez de nb_camera_*) de propósito:
 * o main-controller já tem nb_camera_mode_t/nb_camera_event_t locais no
 * camera_hal/camera_service legado (Layer 1/4, pré-migração dual-MCU). Os
 * dois coexistem até o passo 7 de docs/DM4_CAMERA_MIGRATION.md remover o
 * caminho local; o prefixo extra evita colisão de símbolo nesse meio-tempo.
 */

#define NB_CAMERA_LINK_COMMAND_VERSION 1U
#define NB_CAMERA_LINK_EVENT_VERSION   1U

typedef enum {
    NB_CAMERA_LINK_OP_REQUEST_SNAPSHOT = 1,
    NB_CAMERA_LINK_OP_SET_PREVIEW,
    NB_CAMERA_LINK_OP_SET_MODE,
} nb_camera_link_opcode_t;

typedef enum {
    NB_CAMERA_LINK_PREVIEW_OFF = 0,
    NB_CAMERA_LINK_PREVIEW_ON,
} nb_camera_link_preview_t;

typedef enum {
    NB_CAMERA_LINK_MODE_QQVGA = 0,
    NB_CAMERA_LINK_MODE_QVGA,
} nb_camera_link_mode_t;

typedef enum {
    NB_CAMERA_LINK_STATUS_OK = 0,
    NB_CAMERA_LINK_STATUS_ERROR,
    NB_CAMERA_LINK_STATUS_BUSY,
    NB_CAMERA_LINK_STATUS_UNAVAILABLE,
} nb_camera_link_status_t;

/*
 * Comando semântico do main para o head: nunca carrega pixels. Snapshot/JPEG
 * efetivo viaja depois pelo canal BULK, sob demanda, identificado por
 * request_id. Espelha o desenho de nb_display_command_t.
 */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t opcode;
    uint8_t preview;
    uint8_t mode;
    uint32_t request_id;
    uint8_t reserved[8];
} nb_camera_link_command_t;

_Static_assert(sizeof(nb_camera_link_command_t) == 16U,
               "camera command wire size changed");

/*
 * Evento semântico do head para o main: confirma o resultado do comando e
 * descreve o frame disponível (formato/tamanho), sem anexar dados.
 */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t status;
    uint8_t mode;
    uint8_t reserved;
    uint16_t width;
    uint16_t height;
    uint32_t length;
    uint32_t request_id;
} nb_camera_link_event_t;

_Static_assert(sizeof(nb_camera_link_event_t) == 16U,
               "camera event wire size changed");

bool nb_camera_link_command_is_valid(const nb_camera_link_command_t *command,
                                     size_t length);
bool nb_camera_link_event_is_valid(const nb_camera_link_event_t *event,
                                   size_t length);

#ifdef __cplusplus
}
#endif

#endif /* NB_CAMERA_PROTOCOL_H */
