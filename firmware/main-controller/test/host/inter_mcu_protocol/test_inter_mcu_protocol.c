#include "nb_inter_mcu_protocol.h"
#include "nb_display_protocol.h"
#include "nb_link_wire.h"

#include <stdio.h>
#include <string.h>

static int s_pass;
static int s_fail;

#define TEST(name, expression)                                                \
    do {                                                                      \
        if (expression) {                                                     \
            ++s_pass;                                                         \
        } else {                                                              \
            fprintf(stderr, "FAIL [%s]: %s\n", name, #expression);           \
            ++s_fail;                                                         \
        }                                                                     \
    } while (0)

typedef struct {
    nb_link_frame_header_t header;
    uint8_t payload[32];
} test_frame_t;

static test_frame_t make_frame(void)
{
    test_frame_t frame = {
        .header = {
            .magic = NB_LINK_MAGIC,
            .version_major = NB_LINK_PROTOCOL_VERSION_MAJOR,
            .version_minor = NB_LINK_PROTOCOL_VERSION_MINOR,
            .channel = NB_LINK_CHANNEL_CONTROL,
            .flags = NB_LINK_FLAG_ACK_REQUIRED,
            .message_type = NB_LINK_MSG_DISPLAY_COMMAND,
            .sequence = 42U,
            .payload_length = 4U,
        },
        .payload = {0x10U, 0x20U, 0x30U, 0x40U},
    };
    nb_link_frame_finalize(&frame.header, frame.payload);
    return frame;
}

static void test_crc_vectors(void)
{
    static const char vector[] = "123456789";

    TEST("crc16_ccitt_false",
         nb_link_crc16_ccitt(vector, sizeof(vector) - 1U) == 0x29B1U);
    TEST("crc32_ieee",
         nb_link_crc32_ieee(vector, sizeof(vector) - 1U) == 0xCBF43926U);
    TEST("empty_crc16", nb_link_crc16_ccitt(NULL, 0U) == 0xFFFFU);
    TEST("empty_crc32", nb_link_crc32_ieee(NULL, 0U) == 0U);
}

static void test_valid_and_corrupted_frames(void)
{
    test_frame_t frame = make_frame();
    const size_t size = sizeof(frame.header) + frame.header.payload_length;

    TEST("valid_frame",
         nb_link_frame_validate(&frame, size) == NB_LINK_VALIDATE_OK);
    TEST("valid_header", nb_link_header_is_valid(&frame.header));
    TEST("null_frame",
         nb_link_frame_validate(NULL, 0U) == NB_LINK_VALIDATE_NULL);
    TEST("truncated_header",
         nb_link_frame_validate(&frame, sizeof(frame.header) - 1U) ==
             NB_LINK_VALIDATE_TRUNCATED_HEADER);
    TEST("truncated_payload",
         nb_link_frame_validate(&frame, size - 1U) ==
             NB_LINK_VALIDATE_TRUNCATED_PAYLOAD);

    frame.payload[2] ^= 0x01U;
    TEST("payload_bit_flip",
         nb_link_frame_validate(&frame, size) ==
             NB_LINK_VALIDATE_BAD_PAYLOAD_CRC);
    frame = make_frame();
    frame.header.sequence ^= 0x01U;
    TEST("header_bit_flip",
         nb_link_frame_validate(&frame, size) ==
             NB_LINK_VALIDATE_BAD_HEADER_CRC);
}

static void test_structural_errors(void)
{
    test_frame_t frame = make_frame();
    const size_t size = sizeof(frame.header) + frame.header.payload_length;

    frame.header.magic = 0U;
    TEST("bad_magic",
         nb_link_frame_validate(&frame, size) == NB_LINK_VALIDATE_BAD_MAGIC);

    frame = make_frame();
    frame.header.version_major++;
    TEST("bad_major",
         nb_link_frame_validate(&frame, size) == NB_LINK_VALIDATE_BAD_VERSION);

    frame = make_frame();
    frame.header.channel = NB_LINK_CHANNEL_COUNT;
    TEST("bad_channel",
         nb_link_frame_validate(&frame, size) == NB_LINK_VALIDATE_BAD_CHANNEL);

    frame = make_frame();
    frame.header.payload_length = NB_LINK_MAX_PAYLOAD_BYTES + 1U;
    TEST("oversized_payload",
         nb_link_frame_validate(&frame, sizeof(frame.header)) ==
             NB_LINK_VALIDATE_BAD_LENGTH);
}

static void test_sequence_idempotency(void)
{
    nb_link_sequence_tracker_t tracker;

    nb_link_sequence_tracker_reset(&tracker);
    TEST("first_boot",
         nb_link_sequence_track(&tracker, 100U, NB_LINK_CHANNEL_CONTROL, 7U) ==
             NB_LINK_SEQUENCE_NEW_BOOT);
    TEST("same_sequence_retry",
         nb_link_sequence_track(&tracker, 100U, NB_LINK_CHANNEL_CONTROL, 7U) ==
             NB_LINK_SEQUENCE_RETRY);
    TEST("next_sequence",
         nb_link_sequence_track(&tracker, 100U, NB_LINK_CHANNEL_CONTROL, 8U) ==
             NB_LINK_SEQUENCE_NEW);
    TEST("old_sequence",
         nb_link_sequence_track(&tracker, 100U, NB_LINK_CHANNEL_CONTROL, 6U) ==
             NB_LINK_SEQUENCE_STALE);
    TEST("independent_channel",
         nb_link_sequence_track(&tracker, 100U, NB_LINK_CHANNEL_EVENT, 1U) ==
             NB_LINK_SEQUENCE_NEW);
    TEST("new_boot_resets_sequences",
         nb_link_sequence_track(&tracker, 101U, NB_LINK_CHANNEL_CONTROL, 1U) ==
             NB_LINK_SEQUENCE_NEW_BOOT);
    TEST("post_reboot_retry",
         nb_link_sequence_track(&tracker, 101U, NB_LINK_CHANNEL_CONTROL, 1U) ==
             NB_LINK_SEQUENCE_RETRY);
}

static void test_sequence_wraparound(void)
{
    nb_link_sequence_tracker_t tracker;

    nb_link_sequence_tracker_reset(&tracker);
    (void)nb_link_sequence_track(
        &tracker, 77U, NB_LINK_CHANNEL_BULK, UINT32_MAX);
    TEST("sequence_wrap",
         nb_link_sequence_track(&tracker, 77U, NB_LINK_CHANNEL_BULK, 0U) ==
             NB_LINK_SEQUENCE_NEW);
}

static void test_credits(void)
{
    nb_link_credit_state_t credits;

    nb_link_credit_set(&credits, NB_LINK_BULK_WINDOW_CHUNKS, 8192U);
    TEST("credit_consume", nb_link_credit_try_consume(&credits, 4096U));
    TEST("credit_frame_decrement",
         credits.frame_credits == NB_LINK_BULK_WINDOW_CHUNKS - 1U);
    TEST("credit_bytes_decrement", credits.byte_credits == 4096U);
    TEST("credit_reject_oversize",
         !nb_link_credit_try_consume(&credits, 4097U));

    TEST("credit_consume_last_bytes",
         nb_link_credit_try_consume(&credits, 4096U));
    TEST("credit_reject_zero_bytes_budget",
         !nb_link_credit_try_consume(&credits, 1U));

    nb_link_credit_release(&credits, 4096U);
    TEST("credit_release_frame",
         credits.frame_credits == NB_LINK_BULK_WINDOW_CHUNKS - 1U);
    TEST("credit_release_bytes", credits.byte_credits == 4096U);
}

static void test_wire_packet(void)
{
    test_frame_t source = make_frame();
    const size_t source_size =
        sizeof(source.header) + source.header.payload_length;
    nb_link_wire_packet_t packet;
    const void *frame = NULL;
    size_t frame_length = 0U;

    TEST("wire_pack",
         nb_link_wire_pack(&packet, &source, source_size));
    TEST("wire_unpack",
         nb_link_wire_unpack(&packet, &frame, &frame_length));
    TEST("wire_length", frame_length == source_size);
    TEST("wire_content", memcmp(frame, &source, source_size) == 0);

    packet.magic = 0U;
    TEST("wire_bad_magic",
         !nb_link_wire_unpack(&packet, &frame, &frame_length));
    nb_link_wire_clear(&packet);
    packet.reserved = 1U;
    TEST("wire_reserved_rejected",
         !nb_link_wire_unpack(&packet, &frame, &frame_length));
    TEST("wire_oversize_rejected",
         !nb_link_wire_pack(&packet, &source,
                            NB_LINK_MAX_FRAME_BYTES + 1U));
}

static void test_display_command(void)
{
    nb_display_command_t command = {
        .version = NB_DISPLAY_COMMAND_VERSION,
        .opcode = NB_DISPLAY_OP_SET_SCENE,
        .expression = 2U,
        .brightness = 180U,
        .gaze_x_milli = -250,
        .gaze_y_milli = 400,
        .overlay_flags = 0x0003U,
        .reserved = 0U,
        .generation = 7U,
    };

    TEST("display_valid",
         nb_display_command_is_valid(&command, sizeof(command)));
    TEST("display_null",
         !nb_display_command_is_valid(NULL, sizeof(command)));
    TEST("display_bad_size",
         !nb_display_command_is_valid(&command, sizeof(command) - 1U));

    command.version++;
    TEST("display_bad_version",
         !nb_display_command_is_valid(&command, sizeof(command)));
    command.version = NB_DISPLAY_COMMAND_VERSION;
    command.gaze_x_milli = NB_DISPLAY_GAZE_MAX + 1;
    TEST("display_bad_gaze",
         !nb_display_command_is_valid(&command, sizeof(command)));
    command.gaze_x_milli = 0;
    command.reserved = 1U;
    TEST("display_reserved",
         !nb_display_command_is_valid(&command, sizeof(command)));
}

int main(void)
{
    test_crc_vectors();
    test_valid_and_corrupted_frames();
    test_structural_errors();
    test_sequence_idempotency();
    test_sequence_wraparound();
    test_credits();
    test_wire_packet();
    test_display_command();

    printf("%d/%d testes passaram\n", s_pass, s_pass + s_fail);
    return s_fail == 0 ? 0 : 1;
}
