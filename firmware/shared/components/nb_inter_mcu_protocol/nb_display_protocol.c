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
    if ((command->overlay_flags & ~NB_DISPLAY_OVERLAY_ALL) != 0U) {
        return false;
    }
    return true;
}

bool nb_display_scene_v2_is_valid(const nb_display_scene_v2_t *scene,
                                  size_t length)
{
    if (scene == NULL || length != sizeof(*scene)) {
        return false;
    }
    if (scene->version != NB_DISPLAY_SCENE_V2_VERSION ||
        scene->reserved != 0U) {
        return false;
    }
    if ((scene->block_mask & ~(uint8_t)NB_DISPLAY_V2_BLOCK_ALL) != 0U) {
        return false;
    }
    if ((scene->block_mask & (uint8_t)NB_DISPLAY_V2_BLOCK_BASE) != 0U &&
        scene->expression >= NB_DISPLAY_EXPRESSION_COUNT) {
        return false;
    }
    if (scene->gaze_x_milli < NB_DISPLAY_GAZE_MIN ||
        scene->gaze_x_milli > NB_DISPLAY_GAZE_MAX ||
        scene->gaze_y_milli < NB_DISPLAY_GAZE_MIN ||
        scene->gaze_y_milli > NB_DISPLAY_GAZE_MAX) {
        return false;
    }
    if ((scene->overlay_flags & ~NB_DISPLAY_OVERLAY_ALL) != 0U) {
        return false;
    }
    return true;
}

bool nb_display_transient_v2_is_valid(const nb_display_transient_v2_t *cmd,
                                      size_t length)
{
    if (cmd == NULL || length != sizeof(*cmd)) {
        return false;
    }
    if (cmd->version != NB_DISPLAY_TRANSIENT_V2_VERSION ||
        cmd->reserved8 != 0U) {
        return false;
    }
    if (cmd->op != NB_DISPLAY_TRANSIENT_OP_START &&
        cmd->op != NB_DISPLAY_TRANSIENT_OP_CANCEL) {
        return false;
    }
    if (cmd->kind != NB_DISPLAY_TRANSIENT_KIND_GLANCE) {
        return false;
    }
    if (cmd->transient_id == 0U) {
        return false;
    }
    if (cmd->op == NB_DISPLAY_TRANSIENT_OP_START &&
        (cmd->gaze_x_milli < NB_DISPLAY_GAZE_MIN ||
         cmd->gaze_x_milli > NB_DISPLAY_GAZE_MAX ||
         cmd->gaze_y_milli < NB_DISPLAY_GAZE_MIN ||
         cmd->gaze_y_milli > NB_DISPLAY_GAZE_MAX)) {
        return false;
    }
    return true;
}

bool nb_display_result_v2_is_valid(const nb_display_result_v2_t *result,
                                   size_t length)
{
    if (result == NULL || length != sizeof(*result)) {
        return false;
    }
    if (result->version != NB_DISPLAY_RESULT_V2_VERSION ||
        result->reserved != 0U) {
        return false;
    }
    if (result->status > NB_DISPLAY_RESULT_CANCELLED) {
        return false;
    }
    return true;
}

bool nb_display_transient_v2_is_expired(uint32_t deadline_ms, uint32_t now_ms)
{
    return (int32_t)(now_ms - deadline_ms) > 0;
}

bool nb_display_status_icons_v2_is_valid(
    const nb_display_status_icons_v2_t *icons, size_t length)
{
    if (icons == NULL || length != sizeof(*icons)) {
        return false;
    }
    if (icons->version != NB_DISPLAY_STATUS_ICONS_V2_VERSION) {
        return false;
    }
    if (icons->reserved[0] != 0U || icons->reserved[1] != 0U ||
        icons->reserved[2] != 0U) {
        return false;
    }
    return true;
}
