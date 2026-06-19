#ifndef NB_LINK_WIRE_H
#define NB_LINK_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nb_inter_mcu_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NB_LINK_WIRE_MAGIC       0x4E425750UL
#define NB_LINK_DM1_QUEUE_FRAME_BYTES 256U
#define NB_LINK_MAX_FRAME_BYTES  \
    (sizeof(nb_link_frame_header_t) + NB_LINK_MAX_PAYLOAD_BYTES)

typedef struct {
    uint32_t magic;
    uint16_t frame_length;
    uint16_t reserved;
    uint8_t frame[NB_LINK_MAX_FRAME_BYTES];
} nb_link_wire_packet_t;

_Static_assert(NB_LINK_DM1_QUEUE_FRAME_BYTES >=
                   sizeof(nb_link_frame_header_t),
               "DM1 transport queue must fit at least one frame header");

void nb_link_wire_clear(nb_link_wire_packet_t *packet);
bool nb_link_wire_pack(nb_link_wire_packet_t *packet,
                       const void *frame,
                       size_t frame_length);
bool nb_link_wire_unpack(const nb_link_wire_packet_t *packet,
                         const void **frame,
                         size_t *frame_length);

#ifdef __cplusplus
}
#endif

#endif /* NB_LINK_WIRE_H */
