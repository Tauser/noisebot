#include "nb_inter_mcu_protocol.h"

_Static_assert(sizeof(nb_link_frame_header_t) == 22U,
               "wire header size changed");
_Static_assert(sizeof(nb_link_credit_update_t) == 8U,
               "wire credit payload size changed");
_Static_assert(sizeof(nb_link_time_sync_t) == 24U,
               "wire time payload size changed");
_Static_assert(sizeof(nb_link_event_timestamp_t) == 8U,
               "wire event timestamp size changed");

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
