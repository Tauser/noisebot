#include "nb_link_wire.h"

#include <string.h>

_Static_assert((sizeof(nb_link_wire_packet_t) % 4U) == 0U,
               "SPI wire packet must be word aligned");

void nb_link_wire_clear(nb_link_wire_packet_t *packet)
{
    if (packet != NULL) {
        memset(packet, 0, sizeof(*packet));
        packet->magic = NB_LINK_WIRE_MAGIC;
    }
}

bool nb_link_wire_pack(nb_link_wire_packet_t *packet,
                       const void *frame,
                       size_t frame_length)
{
    if (packet == NULL || frame_length > NB_LINK_MAX_FRAME_BYTES ||
        (frame == NULL && frame_length > 0U)) {
        return false;
    }
    nb_link_wire_clear(packet);
    packet->frame_length = (uint16_t)frame_length;
    if (frame_length > 0U) {
        memcpy(packet->frame, frame, frame_length);
    }
    return true;
}

bool nb_link_wire_unpack(const nb_link_wire_packet_t *packet,
                         const void **frame,
                         size_t *frame_length)
{
    if (packet == NULL || frame == NULL || frame_length == NULL ||
        packet->magic != NB_LINK_WIRE_MAGIC ||
        packet->reserved != 0U ||
        packet->frame_length > NB_LINK_MAX_FRAME_BYTES) {
        return false;
    }
    *frame = packet->frame;
    *frame_length = packet->frame_length;
    return true;
}
