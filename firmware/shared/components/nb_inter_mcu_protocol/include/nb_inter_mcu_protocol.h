#ifndef NB_INTER_MCU_PROTOCOL_H
#define NB_INTER_MCU_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_LINK_MAGIC                  0x4E42U
#define NB_LINK_PROTOCOL_VERSION_MAJOR 1U
#define NB_LINK_PROTOCOL_VERSION_MINOR 1U
#define NB_LINK_MAX_PAYLOAD_BYTES      4096U
#define NB_LINK_BULK_WINDOW_CHUNKS      4U

typedef enum {
    NB_LINK_CHANNEL_CONTROL = 0,
    NB_LINK_CHANNEL_EVENT,
    NB_LINK_CHANNEL_STORAGE,
    NB_LINK_CHANNEL_BULK,
    NB_LINK_CHANNEL_DIAGNOSTIC,
} nb_link_channel_t;

typedef enum {
    NB_LINK_MSG_HELLO = 1,
    NB_LINK_MSG_HELLO_ACK,
    NB_LINK_MSG_HEARTBEAT,
    NB_LINK_MSG_CAPABILITIES,
    NB_LINK_MSG_STATE_SNAPSHOT,
    NB_LINK_MSG_ERROR,
    NB_LINK_MSG_POLL,
    NB_LINK_MSG_IDLE,
    NB_LINK_MSG_CREDIT_UPDATE,
    NB_LINK_MSG_TIME_SYNC,
    NB_LINK_MSG_DISPLAY_COMMAND = 0x100,
    NB_LINK_MSG_TOUCH_EVENT,
    NB_LINK_MSG_CAMERA_COMMAND,
    NB_LINK_MSG_CAMERA_EVENT,
    NB_LINK_MSG_STORAGE_REQUEST = 0x200,
    NB_LINK_MSG_STORAGE_RESPONSE,
    NB_LINK_MSG_STORAGE_STATUS,
    NB_LINK_MSG_BULK_BEGIN = 0x300,
    NB_LINK_MSG_BULK_CHUNK,
    NB_LINK_MSG_BULK_END,
    NB_LINK_MSG_BULK_ABORT,
} nb_link_message_type_t;

typedef enum {
    NB_LINK_FLAG_ACK_REQUIRED = 1U << 0,
    NB_LINK_FLAG_RESPONSE     = 1U << 1,
    NB_LINK_FLAG_ERROR        = 1U << 2,
    NB_LINK_FLAG_RETRY        = 1U << 3,
} nb_link_frame_flag_t;

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t version_major;
    uint8_t version_minor;
    uint8_t channel;
    uint8_t flags;
    uint16_t message_type;
    uint32_t sequence;
    uint32_t payload_length;
    uint32_t payload_crc32;
    uint16_t header_crc16;
} nb_link_frame_header_t;

typedef struct __attribute__((packed)) {
    uint32_t boot_id;
    uint32_t uptime_ms;
    uint32_t capability_bits;
    uint16_t max_payload_bytes;
    uint8_t role;
    uint8_t reserved;
} nb_link_hello_t;

typedef struct __attribute__((packed)) {
    uint8_t channel;
    uint8_t reserved;
    uint16_t frame_credits;
    uint32_t byte_credits;
} nb_link_credit_update_t;

typedef struct __attribute__((packed)) {
    uint64_t main_monotonic_us;
    int64_t unix_time_us;
    uint32_t sync_sequence;
    uint8_t unix_time_valid;
    uint8_t reserved[3];
} nb_link_time_sync_t;

typedef struct __attribute__((packed)) {
    uint32_t head_monotonic_ms;
    uint32_t source_sequence;
} nb_link_event_timestamp_t;

bool nb_link_header_is_valid(const nb_link_frame_header_t *header);

#ifdef __cplusplus
}
#endif

#endif
