#include "nb_inter_mcu_protocol.h"

bool nb_link_header_is_valid(const nb_link_frame_header_t *header)
{
    if (header == NULL) {
        return false;
    }
    if (header->magic != NB_LINK_MAGIC) {
        return false;
    }
    if (header->version_major != NB_LINK_PROTOCOL_VERSION_MAJOR) {
        return false;
    }
    if (header->channel > NB_LINK_CHANNEL_DIAGNOSTIC) {
        return false;
    }
    return header->payload_length <= NB_LINK_MAX_PAYLOAD_BYTES;
}
