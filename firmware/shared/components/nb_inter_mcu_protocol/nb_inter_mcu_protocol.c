#include "nb_inter_mcu_protocol.h"

#include <limits.h>
#include <string.h>

_Static_assert(sizeof(nb_link_frame_header_t) == 22U,
               "wire header size changed");
_Static_assert(sizeof(nb_link_credit_update_t) == 8U,
               "wire credit payload size changed");
_Static_assert(sizeof(nb_link_time_sync_t) == 24U,
               "wire time payload size changed");
_Static_assert(sizeof(nb_link_event_timestamp_t) == 8U,
               "wire event timestamp size changed");

uint16_t nb_link_crc16_ccitt(const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint16_t crc = 0xFFFFU;

    if (bytes == NULL && length > 0U) {
        return 0U;
    }
    for (size_t i = 0U; i < length; ++i) {
        crc ^= (uint16_t)bytes[i] << 8U;
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 0x8000U) != 0U
                      ? (uint16_t)((crc << 1U) ^ 0x1021U)
                      : (uint16_t)(crc << 1U);
        }
    }
    return crc;
}

uint32_t nb_link_crc32_ieee(const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = UINT32_MAX;

    if (bytes == NULL && length > 0U) {
        return 0U;
    }
    for (size_t i = 0U; i < length; ++i) {
        crc ^= bytes[i];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

uint16_t nb_link_header_crc16(const nb_link_frame_header_t *header)
{
    nb_link_frame_header_t copy;

    if (header == NULL) {
        return 0U;
    }
    memcpy(&copy, header, sizeof(copy));
    copy.header_crc16 = 0U;
    return nb_link_crc16_ccitt(&copy, sizeof(copy));
}

void nb_link_frame_finalize(nb_link_frame_header_t *header,
                            const void *payload)
{
    if (header == NULL) {
        return;
    }
    header->payload_crc32 =
        nb_link_crc32_ieee(payload, header->payload_length);
    header->header_crc16 = nb_link_header_crc16(header);
}

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
    if (header->payload_length > NB_LINK_MAX_PAYLOAD_BYTES) {
        return false;
    }
    return header->header_crc16 == nb_link_header_crc16(header);
}

nb_link_validate_result_t nb_link_frame_validate(const void *frame,
                                                 size_t frame_length)
{
    const nb_link_frame_header_t *header;
    const uint8_t *payload;

    if (frame == NULL) {
        return NB_LINK_VALIDATE_NULL;
    }
    if (frame_length < sizeof(nb_link_frame_header_t)) {
        return NB_LINK_VALIDATE_TRUNCATED_HEADER;
    }

    header = (const nb_link_frame_header_t *)frame;
    if (header->magic != NB_LINK_MAGIC) {
        return NB_LINK_VALIDATE_BAD_MAGIC;
    }
    if (header->version_major != NB_LINK_PROTOCOL_VERSION_MAJOR) {
        return NB_LINK_VALIDATE_BAD_VERSION;
    }
    if (header->channel >= NB_LINK_CHANNEL_COUNT) {
        return NB_LINK_VALIDATE_BAD_CHANNEL;
    }
    if (header->payload_length > NB_LINK_MAX_PAYLOAD_BYTES) {
        return NB_LINK_VALIDATE_BAD_LENGTH;
    }
    if (header->header_crc16 != nb_link_header_crc16(header)) {
        return NB_LINK_VALIDATE_BAD_HEADER_CRC;
    }
    if (frame_length - sizeof(*header) < header->payload_length) {
        return NB_LINK_VALIDATE_TRUNCATED_PAYLOAD;
    }

    payload = (const uint8_t *)frame + sizeof(*header);
    if (header->payload_crc32 !=
        nb_link_crc32_ieee(payload, header->payload_length)) {
        return NB_LINK_VALIDATE_BAD_PAYLOAD_CRC;
    }
    return NB_LINK_VALIDATE_OK;
}

void nb_link_sequence_tracker_reset(nb_link_sequence_tracker_t *tracker)
{
    if (tracker != NULL) {
        memset(tracker, 0, sizeof(*tracker));
    }
}

nb_link_sequence_result_t nb_link_sequence_track(
    nb_link_sequence_tracker_t *tracker,
    uint32_t boot_id,
    nb_link_channel_t channel,
    uint32_t sequence)
{
    if (tracker == NULL || (uint32_t)channel >= NB_LINK_CHANNEL_COUNT) {
        return NB_LINK_SEQUENCE_STALE;
    }
    if (!tracker->boot_id_valid || tracker->boot_id != boot_id) {
        nb_link_sequence_tracker_reset(tracker);
        tracker->boot_id = boot_id;
        tracker->boot_id_valid = true;
        tracker->last_sequence[channel] = sequence;
        tracker->seen[channel] = true;
        return NB_LINK_SEQUENCE_NEW_BOOT;
    }
    if (!tracker->seen[channel]) {
        tracker->last_sequence[channel] = sequence;
        tracker->seen[channel] = true;
        return NB_LINK_SEQUENCE_NEW;
    }
    if (tracker->last_sequence[channel] == sequence) {
        return NB_LINK_SEQUENCE_RETRY;
    }
    if ((int32_t)(sequence - tracker->last_sequence[channel]) <= 0) {
        return NB_LINK_SEQUENCE_STALE;
    }
    tracker->last_sequence[channel] = sequence;
    return NB_LINK_SEQUENCE_NEW;
}

void nb_link_credit_set(nb_link_credit_state_t *state,
                        uint16_t frame_credits,
                        uint32_t byte_credits)
{
    if (state != NULL) {
        state->frame_credits = frame_credits;
        state->byte_credits = byte_credits;
    }
}

bool nb_link_credit_try_consume(nb_link_credit_state_t *state,
                                uint32_t payload_bytes)
{
    if (state == NULL || state->frame_credits == 0U ||
        state->byte_credits < payload_bytes) {
        return false;
    }
    --state->frame_credits;
    state->byte_credits -= payload_bytes;
    return true;
}

void nb_link_credit_release(nb_link_credit_state_t *state,
                            uint32_t payload_bytes)
{
    if (state == NULL) {
        return;
    }
    if (state->frame_credits < UINT16_MAX) {
        ++state->frame_credits;
    }
    state->byte_credits =
        UINT32_MAX - state->byte_credits < payload_bytes
            ? UINT32_MAX
            : state->byte_credits + payload_bytes;
}
