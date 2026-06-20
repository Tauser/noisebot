#include "nb_camera_protocol.h"

bool nb_camera_link_command_is_valid(const nb_camera_link_command_t *command,
                                     size_t length)
{
    if (command == NULL || length != sizeof(*command)) {
        return false;
    }
    if (command->version != NB_CAMERA_LINK_COMMAND_VERSION) {
        return false;
    }
    if (command->opcode < NB_CAMERA_LINK_OP_REQUEST_SNAPSHOT ||
        command->opcode > NB_CAMERA_LINK_OP_SET_MODE) {
        return false;
    }
    if (command->opcode == NB_CAMERA_LINK_OP_SET_PREVIEW &&
        command->preview > NB_CAMERA_LINK_PREVIEW_ON) {
        return false;
    }
    if (command->opcode == NB_CAMERA_LINK_OP_SET_MODE &&
        command->mode > NB_CAMERA_LINK_MODE_QVGA) {
        return false;
    }
    for (size_t i = 0U; i < sizeof(command->reserved); ++i) {
        if (command->reserved[i] != 0U) {
            return false;
        }
    }
    return true;
}

bool nb_camera_link_event_is_valid(const nb_camera_link_event_t *event,
                                   size_t length)
{
    if (event == NULL || length != sizeof(*event)) {
        return false;
    }
    if (event->version != NB_CAMERA_LINK_EVENT_VERSION ||
        event->reserved != 0U) {
        return false;
    }
    if (event->status > NB_CAMERA_LINK_STATUS_UNAVAILABLE) {
        return false;
    }
    if (event->mode > NB_CAMERA_LINK_MODE_QVGA) {
        return false;
    }
    if (event->status == NB_CAMERA_LINK_STATUS_OK && event->length == 0U) {
        return false;
    }
    return true;
}
