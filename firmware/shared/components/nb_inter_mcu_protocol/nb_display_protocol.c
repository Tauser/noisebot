#include "nb_display_protocol.h"

bool nb_display_generation_is_newer(uint32_t candidate, uint32_t current)
{
    return (int32_t)(candidate - current) > 0;
}

bool nb_display_command_is_valid(const nb_display_command_t *command,
                                 size_t length)
{
    if (command == NULL || length != sizeof(*command)) {
        return false;
    }
    if (command->version != NB_DISPLAY_COMMAND_VERSION ||
        command->reserved != 0U) {
        return false;
    }
    if (command->opcode < NB_DISPLAY_OP_SET_SCENE ||
        command->opcode > NB_DISPLAY_OP_SET_POWER) {
        return false;
    }
    if (command->gaze_x_milli < NB_DISPLAY_GAZE_MIN ||
        command->gaze_x_milli > NB_DISPLAY_GAZE_MAX ||
        command->gaze_y_milli < NB_DISPLAY_GAZE_MIN ||
        command->gaze_y_milli > NB_DISPLAY_GAZE_MAX) {
        return false;
    }
    if (command->opcode == NB_DISPLAY_OP_SET_POWER &&
        command->expression > NB_DISPLAY_POWER_ON) {
        return false;
    }
    if (command->opcode == NB_DISPLAY_OP_SET_SCENE &&
        command->expression >= NB_DISPLAY_EXPRESSION_COUNT) {
        return false;
    }
    return true;
}
