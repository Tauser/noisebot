#include "nb_inter_mcu_protocol.h"
#include "nb_display_protocol.h"
#include "nb_camera_protocol.h"
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
    command.overlay_flags = NB_DISPLAY_OVERLAY_ALL | (1U << 8);
    TEST("display_bad_overlay",
         !nb_display_command_is_valid(&command, sizeof(command)));
    command.overlay_flags = NB_DISPLAY_OVERLAY_LISTENING |
                            NB_DISPLAY_OVERLAY_SPEAKING;
    command.reserved = 1U;
    TEST("display_reserved",
         !nb_display_command_is_valid(&command, sizeof(command)));

    TEST("display_generation_newer",
         nb_display_generation_is_newer(8U, 7U));
    TEST("display_generation_duplicate",
         !nb_display_generation_is_newer(7U, 7U));
    TEST("display_generation_stale",
         !nb_display_generation_is_newer(6U, 7U));
    TEST("display_generation_wrap",
         nb_display_generation_is_newer(0U, UINT32_MAX));
}

static void test_display_scene_v2(void)
{
    nb_display_scene_v2_t scene = {
        .version = NB_DISPLAY_SCENE_V2_VERSION,
        .block_mask = NB_DISPLAY_V2_BLOCK_BASE,
        .expression = 3U,
        .brightness = 200U,
        .gaze_x_milli = -500,
        .gaze_y_milli = 600,
        .overlay_flags = NB_DISPLAY_OVERLAY_LISTENING,
        .reserved = 0U,
        .generation = 12U,
    };

    TEST("scene_v2_valid", nb_display_scene_v2_is_valid(&scene, sizeof(scene)));
    TEST("scene_v2_null", !nb_display_scene_v2_is_valid(NULL, sizeof(scene)));
    TEST("scene_v2_bad_size",
         !nb_display_scene_v2_is_valid(&scene, sizeof(scene) - 1U));

    scene.version++;
    TEST("scene_v2_bad_version",
         !nb_display_scene_v2_is_valid(&scene, sizeof(scene)));
    scene.version = NB_DISPLAY_SCENE_V2_VERSION;

    scene.reserved = 1U;
    TEST("scene_v2_reserved",
         !nb_display_scene_v2_is_valid(&scene, sizeof(scene)));
    scene.reserved = 0U;

    scene.block_mask = (uint8_t)(NB_DISPLAY_V2_BLOCK_ALL + 1U);
    TEST("scene_v2_bad_block_mask",
         !nb_display_scene_v2_is_valid(&scene, sizeof(scene)));
    scene.block_mask = NB_DISPLAY_V2_BLOCK_BASE;

    scene.expression = NB_DISPLAY_EXPRESSION_COUNT;
    TEST("scene_v2_bad_expression_when_base_present",
         !nb_display_scene_v2_is_valid(&scene, sizeof(scene)));
    /* Sem o bloco BASE presente, expressao fora do catalogo nao importa
     * ainda -- campo pertence a um bloco que o receptor deve ignorar. */
    scene.block_mask = 0U;
    TEST("scene_v2_no_block_ignores_expression",
         nb_display_scene_v2_is_valid(&scene, sizeof(scene)));
    scene.block_mask = NB_DISPLAY_V2_BLOCK_BASE;
    scene.expression = 3U;

    scene.gaze_x_milli = NB_DISPLAY_GAZE_MAX + 1;
    TEST("scene_v2_bad_gaze",
         !nb_display_scene_v2_is_valid(&scene, sizeof(scene)));
    scene.gaze_x_milli = -500;

    scene.overlay_flags = NB_DISPLAY_OVERLAY_ALL | (1U << 8);
    TEST("scene_v2_bad_overlay",
         !nb_display_scene_v2_is_valid(&scene, sizeof(scene)));
}

static void test_display_transient_v2(void)
{
    nb_display_transient_v2_t cmd = {
        .version = NB_DISPLAY_TRANSIENT_V2_VERSION,
        .op = NB_DISPLAY_TRANSIENT_OP_START,
        .kind = NB_DISPLAY_TRANSIENT_KIND_GLANCE,
        .reserved8 = 0U,
        .transient_id = 1U,
        .gaze_x_milli = -300,
        .gaze_y_milli = 200,
        .deadline_ms = 5000U,
    };

    TEST("transient_v2_valid",
         nb_display_transient_v2_is_valid(&cmd, sizeof(cmd)));
    TEST("transient_v2_null",
         !nb_display_transient_v2_is_valid(NULL, sizeof(cmd)));
    TEST("transient_v2_bad_size",
         !nb_display_transient_v2_is_valid(&cmd, sizeof(cmd) - 1U));

    cmd.version++;
    TEST("transient_v2_bad_version",
         !nb_display_transient_v2_is_valid(&cmd, sizeof(cmd)));
    cmd.version = NB_DISPLAY_TRANSIENT_V2_VERSION;

    cmd.reserved8 = 1U;
    TEST("transient_v2_reserved",
         !nb_display_transient_v2_is_valid(&cmd, sizeof(cmd)));
    cmd.reserved8 = 0U;

    cmd.transient_id = 0U;
    TEST("transient_v2_id_zero_invalid",
         !nb_display_transient_v2_is_valid(&cmd, sizeof(cmd)));
    cmd.transient_id = 1U;

    cmd.op = 0U;
    TEST("transient_v2_bad_op",
         !nb_display_transient_v2_is_valid(&cmd, sizeof(cmd)));
    cmd.op = NB_DISPLAY_TRANSIENT_OP_START;

    cmd.kind = 0U;
    TEST("transient_v2_bad_kind",
         !nb_display_transient_v2_is_valid(&cmd, sizeof(cmd)));
    cmd.kind = NB_DISPLAY_TRANSIENT_KIND_GLANCE;

    cmd.gaze_x_milli = NB_DISPLAY_GAZE_MIN - 1;
    TEST("transient_v2_bad_gaze_on_start",
         !nb_display_transient_v2_is_valid(&cmd, sizeof(cmd)));
    cmd.gaze_x_milli = -300;

    /* CANCEL nao carrega gaze valido -- nao deveria ser validado contra
     * a faixa de gaze, o cancelamento so precisa do transient_id. */
    cmd.op = NB_DISPLAY_TRANSIENT_OP_CANCEL;
    cmd.gaze_x_milli = (int16_t)(NB_DISPLAY_GAZE_MIN - 1);
    TEST("transient_v2_cancel_ignores_gaze",
         nb_display_transient_v2_is_valid(&cmd, sizeof(cmd)));

    TEST("transient_v2_not_expired",
         !nb_display_transient_v2_is_expired(5000U, 4999U));
    TEST("transient_v2_expired",
         nb_display_transient_v2_is_expired(5000U, 5001U));
    TEST("transient_v2_deadline_exact_not_expired",
         !nb_display_transient_v2_is_expired(5000U, 5000U));
}

static void test_display_result_v2(void)
{
    nb_display_result_v2_t result = {
        .version = NB_DISPLAY_RESULT_V2_VERSION,
        .status = NB_DISPLAY_RESULT_OK,
        .reserved = 0U,
        .transient_id = 1U,
        .generation = 0U,
    };

    TEST("result_v2_valid",
         nb_display_result_v2_is_valid(&result, sizeof(result)));
    TEST("result_v2_null",
         !nb_display_result_v2_is_valid(NULL, sizeof(result)));
    TEST("result_v2_bad_size",
         !nb_display_result_v2_is_valid(&result, sizeof(result) - 1U));

    result.version++;
    TEST("result_v2_bad_version",
         !nb_display_result_v2_is_valid(&result, sizeof(result)));
    result.version = NB_DISPLAY_RESULT_V2_VERSION;

    result.reserved = 1U;
    TEST("result_v2_reserved",
         !nb_display_result_v2_is_valid(&result, sizeof(result)));
    result.reserved = 0U;

    result.status = NB_DISPLAY_RESULT_CANCELLED + 1U;
    TEST("result_v2_bad_status",
         !nb_display_result_v2_is_valid(&result, sizeof(result)));
    result.status = NB_DISPLAY_RESULT_EXPIRED;
    TEST("result_v2_expired_status_ok",
         nb_display_result_v2_is_valid(&result, sizeof(result)));
}

static void test_camera_command(void)
{
    nb_camera_link_command_t command = {
        .version = NB_CAMERA_LINK_COMMAND_VERSION,
        .opcode = NB_CAMERA_LINK_OP_REQUEST_SNAPSHOT,
        .preview = 0U,
        .mode = NB_CAMERA_LINK_MODE_QQVGA,
        .request_id = 5U,
    };

    TEST("camera_cmd_valid",
         nb_camera_link_command_is_valid(&command, sizeof(command)));
    TEST("camera_cmd_null",
         !nb_camera_link_command_is_valid(NULL, sizeof(command)));
    TEST("camera_cmd_bad_size",
         !nb_camera_link_command_is_valid(&command, sizeof(command) - 1U));

    command.version++;
    TEST("camera_cmd_bad_version",
         !nb_camera_link_command_is_valid(&command, sizeof(command)));
    command.version = NB_CAMERA_LINK_COMMAND_VERSION;

    command.opcode = NB_CAMERA_LINK_OP_SET_PREVIEW;
    command.preview = NB_CAMERA_LINK_PREVIEW_ON;
    TEST("camera_cmd_preview_on",
         nb_camera_link_command_is_valid(&command, sizeof(command)));
    command.preview = NB_CAMERA_LINK_PREVIEW_ON + 1U;
    TEST("camera_cmd_bad_preview",
         !nb_camera_link_command_is_valid(&command, sizeof(command)));

    command.opcode = NB_CAMERA_LINK_OP_SET_MODE;
    command.preview = 0U;
    command.mode = NB_CAMERA_LINK_MODE_QVGA;
    TEST("camera_cmd_mode_qvga",
         nb_camera_link_command_is_valid(&command, sizeof(command)));
    command.mode = NB_CAMERA_LINK_MODE_QVGA + 1U;
    TEST("camera_cmd_bad_mode",
         !nb_camera_link_command_is_valid(&command, sizeof(command)));
    command.mode = NB_CAMERA_LINK_MODE_QQVGA;
    command.reserved[0] = 1U;
    TEST("camera_cmd_reserved",
         !nb_camera_link_command_is_valid(&command, sizeof(command)));
}

static void test_camera_event(void)
{
    nb_camera_link_event_t event = {
        .version = NB_CAMERA_LINK_EVENT_VERSION,
        .status = NB_CAMERA_LINK_STATUS_OK,
        .mode = NB_CAMERA_LINK_MODE_QQVGA,
        .reserved = 0U,
        .width = 160U,
        .height = 120U,
        .length = 4096U,
        .request_id = 5U,
    };

    TEST("camera_evt_valid",
         nb_camera_link_event_is_valid(&event, sizeof(event)));
    TEST("camera_evt_null",
         !nb_camera_link_event_is_valid(NULL, sizeof(event)));
    TEST("camera_evt_bad_size",
         !nb_camera_link_event_is_valid(&event, sizeof(event) - 1U));

    event.reserved = 1U;
    TEST("camera_evt_reserved",
         !nb_camera_link_event_is_valid(&event, sizeof(event)));
    event.reserved = 0U;

    event.status = NB_CAMERA_LINK_STATUS_UNAVAILABLE + 1U;
    TEST("camera_evt_bad_status",
         !nb_camera_link_event_is_valid(&event, sizeof(event)));
    event.status = NB_CAMERA_LINK_STATUS_OK;

    event.mode = NB_CAMERA_LINK_MODE_QVGA + 1U;
    TEST("camera_evt_bad_mode",
         !nb_camera_link_event_is_valid(&event, sizeof(event)));
    event.mode = NB_CAMERA_LINK_MODE_QQVGA;

    event.length = 0U;
    TEST("camera_evt_ok_needs_length",
         !nb_camera_link_event_is_valid(&event, sizeof(event)));
    event.status = NB_CAMERA_LINK_STATUS_UNAVAILABLE;
    TEST("camera_evt_unavailable_zero_length",
         nb_camera_link_event_is_valid(&event, sizeof(event)));
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
    test_display_scene_v2();
    test_display_transient_v2();
    test_display_result_v2();
    test_camera_command();
    test_camera_event();

    printf("%d/%d testes passaram\n", s_pass, s_pass + s_fail);
    return s_fail == 0 ? 0 : 1;
}
