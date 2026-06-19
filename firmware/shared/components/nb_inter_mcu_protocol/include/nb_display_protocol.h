#ifndef NB_DISPLAY_PROTOCOL_H
#define NB_DISPLAY_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_DISPLAY_COMMAND_VERSION 1U
#define NB_DISPLAY_GAZE_MIN       (-1000)
#define NB_DISPLAY_GAZE_MAX       1000

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

#ifdef __cplusplus
}
#endif

#endif /* NB_DISPLAY_PROTOCOL_H */
