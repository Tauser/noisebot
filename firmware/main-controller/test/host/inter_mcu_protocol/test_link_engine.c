/*
 * Testes de host do nb_link_engine (DM1) com transporte simulado.
 * Dois engines (main/head) conversam por um "wire" em memória que permite
 * drop e partição para fault injection. Nenhum hardware envolvido.
 */

#include "nb_link_engine.h"

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

/* ── Wire simulado ───────────────────────────────────────────────────────── */

typedef struct {
    uint8_t data[256];
    size_t len;
} sim_frame_t;

typedef struct {
    sim_frame_t q[64];
    int count;
} sim_fifo_t;

typedef struct {
    sim_fifo_t to_a; /* frames destinados a A (enviados por B) */
    sim_fifo_t to_b; /* frames destinados a B (enviados por A) */
    int drop_a_to_b; /* descarta os próximos N frames A→B       */
    int drop_b_to_a;
    bool partition;  /* corta o link nas duas direções          */
    bool blocked;    /* transporte ocupado: send retorna false  */
} sim_wire_t;

typedef struct {
    sim_wire_t *wire;
    bool is_a;
} sim_endpoint_t;

static void fifo_push(sim_fifo_t *f, const void *data, size_t len)
{
    if (f->count >= (int)(sizeof(f->q) / sizeof(f->q[0])) ||
        len > sizeof(f->q[0].data)) {
        return;
    }
    memcpy(f->q[f->count].data, data, len);
    f->q[f->count].len = len;
    ++f->count;
}

static bool fifo_pop(sim_fifo_t *f, sim_frame_t *out)
{
    if (f->count == 0) {
        return false;
    }
    *out = f->q[0];
    for (int i = 1; i < f->count; ++i) {
        f->q[i - 1] = f->q[i];
    }
    --f->count;
    return true;
}

static bool sim_send(void *ctx, const void *frame, size_t length)
{
    sim_endpoint_t *ep = (sim_endpoint_t *)ctx;
    sim_wire_t *w = ep->wire;
    if (w->blocked) {
        return false;
    }
    if (w->partition) {
        return true; /* "enviado", mas perdido no fio */
    }
    if (ep->is_a) {
        if (w->drop_a_to_b > 0) {
            --w->drop_a_to_b;
            return true;
        }
        fifo_push(&w->to_b, frame, length);
    } else {
        if (w->drop_b_to_a > 0) {
            --w->drop_b_to_a;
            return true;
        }
        fifo_push(&w->to_a, frame, length);
    }
    return true;
}

/* Drena o wire até assentar (entrega respostas geradas no meio do caminho). */
static void wire_step(sim_wire_t *w, nb_link_engine_t *ea, nb_link_engine_t *eb)
{
    for (int i = 0; i < 128; ++i) {
        bool any = false;
        sim_frame_t f;
        if (fifo_pop(&w->to_b, &f)) {
            nb_link_engine_on_frame(eb, f.data, f.len);
            any = true;
        }
        if (fifo_pop(&w->to_a, &f)) {
            nb_link_engine_on_frame(ea, f.data, f.len);
            any = true;
        }
        if (!any) {
            break;
        }
    }
}

/* ── App sink ────────────────────────────────────────────────────────────── */

typedef struct {
    int count;
    uint16_t last_type;
    uint8_t last_payload[64];
    uint16_t last_len;
    int tx_acked;
    int tx_timeout;
    int tx_aborted;
    uint32_t last_tx_sequence;
} app_sink_t;

static void on_message(void *ctx, nb_link_channel_t channel,
                       uint16_t message_type, const void *payload,
                       uint16_t length)
{
    (void)channel;
    app_sink_t *sink = (app_sink_t *)ctx;
    ++sink->count;
    sink->last_type = message_type;
    sink->last_len = length;
    if (length <= sizeof(sink->last_payload) && payload != NULL) {
        memcpy(sink->last_payload, payload, length);
    }
}

static void on_tx_result(void *ctx, nb_link_tx_result_t result,
                         nb_link_channel_t channel, uint16_t message_type,
                         uint32_t sequence)
{
    (void)channel;
    (void)message_type;
    app_sink_t *sink = (app_sink_t *)ctx;
    sink->last_tx_sequence = sequence;
    if (result == NB_LINK_TX_ACKED) {
        ++sink->tx_acked;
    } else if (result == NB_LINK_TX_TIMEOUT) {
        ++sink->tx_timeout;
    } else if (result == NB_LINK_TX_ABORTED_PEER_REBOOT) {
        ++sink->tx_aborted;
    }
}

/* ── Harness ─────────────────────────────────────────────────────────────── */

typedef struct {
    sim_wire_t wire;
    sim_endpoint_t ep_a;
    sim_endpoint_t ep_b;
    nb_link_engine_t main_e;
    nb_link_engine_t head_e;
    app_sink_t main_sink;
    app_sink_t head_sink;
    uint32_t clock;
} fixture_t;

static void fixture_init(fixture_t *fx, uint32_t main_boot, uint32_t head_boot)
{
    memset(fx, 0, sizeof(*fx));
    fx->ep_a.wire = &fx->wire;
    fx->ep_a.is_a = true;
    fx->ep_b.wire = &fx->wire;
    fx->ep_b.is_a = false;

    nb_link_engine_config_t main_cfg = {
        .role = NB_LINK_ROLE_MAIN,
        .boot_id = main_boot,
        .version_major = 1U,
        .version_minor = 1U,
        .transport = {.send = sim_send, .ctx = &fx->ep_a},
        .on_message = on_message,
        .on_tx_result = on_tx_result,
        .user_ctx = &fx->main_sink,
    };
    nb_link_engine_config_t head_cfg = {
        .role = NB_LINK_ROLE_HEAD,
        .boot_id = head_boot,
        .version_major = 1U,
        .version_minor = 1U,
        .transport = {.send = sim_send, .ctx = &fx->ep_b},
        .on_message = on_message,
        .on_tx_result = on_tx_result,
        .user_ctx = &fx->head_sink,
    };
    nb_link_engine_init(&fx->main_e, &main_cfg);
    nb_link_engine_init(&fx->head_e, &head_cfg);
}

static void fixture_start(fixture_t *fx)
{
    nb_link_engine_start(&fx->main_e, fx->clock);
    nb_link_engine_start(&fx->head_e, fx->clock);
    wire_step(&fx->wire, &fx->main_e, &fx->head_e);
}

static void fixture_run(fixture_t *fx, uint32_t dt, int steps)
{
    for (int i = 0; i < steps; ++i) {
        fx->clock += dt;
        nb_link_engine_tick(&fx->main_e, fx->clock);
        nb_link_engine_tick(&fx->head_e, fx->clock);
        wire_step(&fx->wire, &fx->main_e, &fx->head_e);
    }
}

static size_t make_app_frame(uint8_t *buf, uint8_t channel, uint16_t type,
                             uint32_t seq, const void *payload, uint16_t len)
{
    nb_link_frame_header_t header = {
        .magic = NB_LINK_MAGIC,
        .version_major = 1U,
        .version_minor = 1U,
        .channel = channel,
        .flags = 0U,
        .message_type = type,
        .sequence = seq,
        .payload_length = len,
    };
    nb_link_frame_finalize(&header, payload);
    memcpy(buf, &header, sizeof(header));
    if (len > 0U) {
        memcpy(buf + sizeof(header), payload, len);
    }
    return sizeof(header) + len;
}

/* ── Casos ───────────────────────────────────────────────────────────────── */

static void test_handshake(void)
{
    fixture_t fx;
    fixture_init(&fx, 1001U, 2002U);
    fixture_start(&fx);

    TEST("main_ready", nb_link_engine_is_operational(&fx.main_e));
    TEST("head_ready", nb_link_engine_is_operational(&fx.head_e));
    TEST("main_ready_once",
         nb_link_engine_stats(&fx.main_e)->ready_transitions == 1U);
    TEST("head_ready_once",
         nb_link_engine_stats(&fx.head_e)->ready_transitions == 1U);
}

static void test_heartbeat_keeps_alive(void)
{
    fixture_t fx;
    fixture_init(&fx, 1U, 2U);
    fixture_start(&fx);

    fixture_run(&fx, 250U, 20); /* 5 s */

    TEST("still_ready_main", nb_link_engine_is_operational(&fx.main_e));
    TEST("still_ready_head", nb_link_engine_is_operational(&fx.head_e));
    TEST("heartbeats_flowed",
         nb_link_engine_stats(&fx.main_e)->heartbeats_tx > 0U &&
             nb_link_engine_stats(&fx.head_e)->heartbeats_rx > 0U);
    TEST("no_link_drops",
         nb_link_engine_stats(&fx.main_e)->link_drops == 0U);
}

static void test_snapshot_loss_retries_before_ready(void)
{
    fixture_t fx;
    fixture_init(&fx, 10U, 20U);

    /*
     * A→B: HELLO é o primeiro frame e STATE_SNAPSHOT é o segundo.
     * Entrega HELLO normalmente e descarta o snapshot inicial.
     */
    nb_link_engine_start(&fx.main_e, fx.clock);
    nb_link_engine_start(&fx.head_e, fx.clock);

    sim_frame_t hello;
    TEST("hello_queued", fifo_pop(&fx.wire.to_b, &hello));
    nb_link_engine_on_frame(&fx.head_e, hello.data, hello.len);
    fx.wire.drop_a_to_b = 1;
    wire_step(&fx.wire, &fx.main_e, &fx.head_e);

    TEST("main_waits_snapshot_ack",
         nb_link_engine_state(&fx.main_e) == NB_LINK_STATE_SNAPSHOT);
    TEST("head_waits_snapshot",
         nb_link_engine_state(&fx.head_e) == NB_LINK_STATE_SNAPSHOT);
    TEST("not_ready_after_snapshot_loss",
         !nb_link_engine_is_operational(&fx.main_e) &&
             !nb_link_engine_is_operational(&fx.head_e));

    fixture_run(&fx, NB_LINK_HANDSHAKE_RETRY_MS, 2);
    TEST("main_ready_after_snapshot_retry",
         nb_link_engine_is_operational(&fx.main_e));
    TEST("head_ready_after_snapshot_retry",
         nb_link_engine_is_operational(&fx.head_e));
}

static void test_timeout_then_recover(void)
{
    fixture_t fx;
    fixture_init(&fx, 1U, 2U);
    fixture_start(&fx);
    fixture_run(&fx, 250U, 4); /* assenta READY */

    TEST("ready_before_cut", nb_link_engine_is_operational(&fx.main_e));

    fx.wire.partition = true;
    fixture_run(&fx, 250U, 8); /* 2 s sem heartbeat */
    TEST("main_degraded",
         nb_link_engine_state(&fx.main_e) == NB_LINK_STATE_DEGRADED);
    TEST("head_degraded",
         nb_link_engine_state(&fx.head_e) == NB_LINK_STATE_DEGRADED);
    TEST("link_drop_counted",
         nb_link_engine_stats(&fx.main_e)->link_drops >= 1U);

    fx.wire.partition = false;
    fixture_run(&fx, 250U, 8);
    TEST("main_recovered", nb_link_engine_is_operational(&fx.main_e));
    TEST("head_recovered", nb_link_engine_is_operational(&fx.head_e));
}

static void test_app_delivery_and_dedup(void)
{
    fixture_t fx;
    fixture_init(&fx, 7U, 9U);
    fixture_start(&fx);

    const uint8_t body[3] = {0xAAU, 0xBBU, 0xCCU};
    bool sent = nb_link_engine_send(&fx.main_e, NB_LINK_CHANNEL_CONTROL,
                                    NB_LINK_MSG_DISPLAY_COMMAND, body,
                                    sizeof(body));
    wire_step(&fx.wire, &fx.main_e, &fx.head_e);

    TEST("app_sent", sent);
    TEST("app_delivered_once", fx.head_sink.count == 1);
    TEST("app_type_ok", fx.head_sink.last_type == NB_LINK_MSG_DISPLAY_COMMAND);
    TEST("app_payload_ok",
         fx.head_sink.last_len == sizeof(body) &&
             memcmp(fx.head_sink.last_payload, body, sizeof(body)) == 0);
    TEST("app_acknowledged",
         nb_link_engine_stats(&fx.main_e)->acked_tx == 1U);
    TEST("app_ack_callback", fx.main_sink.tx_acked == 1);

    /* Reentrega manual do MESMO frame (mesma sequence) → dedupe. */
    uint8_t dup[64];
    const uint8_t evt[2] = {0x01U, 0x02U};
    size_t n = make_app_frame(dup, (uint8_t)NB_LINK_CHANNEL_EVENT,
                              NB_LINK_MSG_TOUCH_EVENT, 5000U, evt, sizeof(evt));
    nb_link_engine_on_frame(&fx.head_e, dup, n);
    int after_first = fx.head_sink.count;
    nb_link_engine_on_frame(&fx.head_e, dup, n);

    TEST("event_delivered_once", fx.head_sink.count == after_first);
    TEST("retry_counted",
         nb_link_engine_stats(&fx.head_e)->retries_rx >= 1U);
}

static void test_ack_latency_telemetry(void)
{
    fixture_t fx;
    fixture_init(&fx, 21U, 22U);
    fixture_start(&fx);

    const uint8_t body = 0x42U;
    TEST("latency_send_accepted",
         nb_link_engine_send(&fx.main_e, NB_LINK_CHANNEL_CONTROL,
                             NB_LINK_MSG_DISPLAY_COMMAND,
                             &body, sizeof(body)));

    sim_frame_t app;
    TEST("latency_app_queued", fifo_pop(&fx.wire.to_b, &app));
    nb_link_engine_on_frame(&fx.head_e, app.data, app.len);

    fx.clock += 37U;
    nb_link_engine_tick(&fx.main_e, fx.clock);
    nb_link_engine_tick(&fx.head_e, fx.clock);
    wire_step(&fx.wire, &fx.main_e, &fx.head_e);

    const nb_link_engine_stats_t *stats =
        nb_link_engine_stats(&fx.main_e);
    TEST("ack_latency_sampled", stats->ack_rtt_samples == 1U);
    TEST("ack_latency_last", stats->ack_rtt_last_ms == 37U);
    TEST("ack_latency_max", stats->ack_rtt_max_ms == 37U);
    TEST("ack_latency_total", stats->ack_rtt_total_ms == 37U);
    TEST("ack_latency_e2e", stats->ack_e2e_last_ms == 37U);
}

static void test_lost_ack_retries_same_sequence(void)
{
    fixture_t fx;
    fixture_init(&fx, 30U, 40U);
    fixture_start(&fx);

    fx.wire.drop_b_to_a = 1; /* perde o primeiro ACK head→main */
    const uint8_t body[2] = {0xA5U, 0x5AU};
    TEST("reliable_send_accepted",
         nb_link_engine_send(&fx.main_e, NB_LINK_CHANNEL_CONTROL,
                             NB_LINK_MSG_DISPLAY_COMMAND, body,
                             sizeof(body)));
    wire_step(&fx.wire, &fx.main_e, &fx.head_e);
    TEST("delivered_before_ack_retry", fx.head_sink.count == 1);
    TEST("not_acked_after_drop",
         nb_link_engine_stats(&fx.main_e)->acked_tx == 0U);

    fixture_run(&fx, NB_LINK_ACK_RETRY_MS, 2);
    TEST("not_delivered_twice", fx.head_sink.count == 1);
    TEST("retry_seen_by_receiver",
         nb_link_engine_stats(&fx.head_e)->retries_rx >= 1U);
    TEST("ack_received_after_retry",
         nb_link_engine_stats(&fx.main_e)->acked_tx == 1U);
    TEST("same_sequence_retried",
         nb_link_engine_stats(&fx.main_e)->ack_retries_tx >= 1U);
}

static void test_pending_aborted_on_peer_reboot(void)
{
    fixture_t fx;
    fixture_init(&fx, 50U, 60U);
    fixture_start(&fx);

    fx.wire.drop_b_to_a = 1;
    const uint8_t body = 0x7EU;
    TEST("pending_before_reboot",
         nb_link_engine_send(&fx.main_e, NB_LINK_CHANNEL_CONTROL,
                             NB_LINK_MSG_DISPLAY_COMMAND, &body, sizeof(body)));
    wire_step(&fx.wire, &fx.main_e, &fx.head_e);

    nb_link_engine_config_t head_cfg = {
        .role = NB_LINK_ROLE_HEAD,
        .boot_id = 61U,
        .version_major = NB_LINK_PROTOCOL_VERSION_MAJOR,
        .version_minor = NB_LINK_PROTOCOL_VERSION_MINOR,
        .transport = {.send = sim_send, .ctx = &fx.ep_b},
        .on_message = on_message,
        .on_tx_result = on_tx_result,
        .user_ctx = &fx.head_sink,
    };
    nb_link_engine_init(&fx.head_e, &head_cfg);
    nb_link_engine_start(&fx.head_e, fx.clock);

    /* Main força novo HELLO para descobrir o boot_id novo do head. */
    nb_link_engine_start(&fx.main_e, fx.clock);
    wire_step(&fx.wire, &fx.main_e, &fx.head_e);

    TEST("pending_aborted_on_reboot",
         nb_link_engine_stats(&fx.main_e)->aborted_on_reboot >= 1U);
    TEST("reboot_abort_callback", fx.main_sink.tx_aborted >= 1);
    TEST("ready_after_head_reboot",
         nb_link_engine_is_operational(&fx.main_e) &&
             nb_link_engine_is_operational(&fx.head_e));
}

static void test_ack_timeout_is_explicit(void)
{
    fixture_t fx;
    fixture_init(&fx, 70U, 80U);
    fixture_start(&fx);

    fx.wire.partition = true;
    const uint8_t body = 0x55U;
    TEST("timeout_send_accepted",
         nb_link_engine_send(&fx.main_e, NB_LINK_CHANNEL_CONTROL,
                             NB_LINK_MSG_DISPLAY_COMMAND, &body, sizeof(body)));
    fixture_run(&fx, NB_LINK_ACK_RETRY_MS, NB_LINK_ACK_MAX_RETRIES + 2);

    TEST("ack_timeout_counted",
         nb_link_engine_stats(&fx.main_e)->ack_timeouts == 1U);
    TEST("ack_timeout_callback", fx.main_sink.tx_timeout == 1);
    TEST("ack_retries_bounded",
         nb_link_engine_stats(&fx.main_e)->ack_retries_tx ==
             NB_LINK_ACK_MAX_RETRIES);
}

static void test_control_preempts_bulk(void)
{
    fixture_t fx;
    fixture_init(&fx, 90U, 91U);
    fixture_start(&fx);
    nb_link_engine_set_bulk_credits(&fx.main_e, 1U, 64U);

    fx.wire.blocked = true;
    const uint8_t bulk = 0xB0U;
    const uint8_t control = 0xC0U;
    TEST("bulk_queued_while_blocked",
         nb_link_engine_send(&fx.main_e, NB_LINK_CHANNEL_BULK,
                             NB_LINK_MSG_BULK_CHUNK, &bulk, sizeof(bulk)));
    TEST("control_queued_while_blocked",
         nb_link_engine_send(&fx.main_e, NB_LINK_CHANNEL_CONTROL,
                             NB_LINK_MSG_DISPLAY_COMMAND, &control,
                             sizeof(control)));

    fx.wire.blocked = false;
    nb_link_engine_tick(&fx.main_e, fx.clock);

    TEST("two_frames_pumped", fx.wire.to_b.count == 2);
    if (fx.wire.to_b.count == 2) {
        nb_link_frame_header_t first;
        nb_link_frame_header_t second;
        memcpy(&first, fx.wire.to_b.q[0].data, sizeof(first));
        memcpy(&second, fx.wire.to_b.q[1].data, sizeof(second));
        TEST("control_preempts_bulk",
             first.channel == NB_LINK_CHANNEL_CONTROL &&
                 second.channel == NB_LINK_CHANNEL_BULK);
    }
    wire_step(&fx.wire, &fx.main_e, &fx.head_e);
}

static void test_bulk_backpressure(void)
{
    fixture_t fx;
    fixture_init(&fx, 3U, 4U);
    fixture_start(&fx);

    const uint8_t chunk[16] = {0};
    bool no_credit = nb_link_engine_send(&fx.main_e, NB_LINK_CHANNEL_BULK,
                                         NB_LINK_MSG_BULK_CHUNK, chunk,
                                         sizeof(chunk));
    TEST("bulk_rejected_without_credit", !no_credit);
    TEST("backpressure_counted",
         nb_link_engine_stats(&fx.main_e)->tx_backpressure >= 1U);

    nb_link_engine_set_bulk_credits(&fx.main_e, 4U, 4096U);
    bool with_credit = nb_link_engine_send(&fx.main_e, NB_LINK_CHANNEL_BULK,
                                           NB_LINK_MSG_BULK_CHUNK, chunk,
                                           sizeof(chunk));
    TEST("bulk_accepted_with_credit", with_credit);
}

static void test_send_before_ready(void)
{
    fixture_t fx;
    fixture_init(&fx, 5U, 6U);
    /* sem start: estado RESET */
    bool sent = nb_link_engine_send(&fx.main_e, NB_LINK_CHANNEL_CONTROL,
                                    NB_LINK_MSG_DISPLAY_COMMAND, NULL, 0U);
    TEST("send_blocked_before_ready", !sent);
    TEST("backpressure_before_ready",
         nb_link_engine_stats(&fx.main_e)->tx_backpressure >= 1U);
}

static void test_corrupt_frame_dropped(void)
{
    fixture_t fx;
    fixture_init(&fx, 5U, 6U);
    fixture_start(&fx);

    uint8_t frame[64];
    const uint8_t body[2] = {0x10U, 0x20U};
    size_t n = make_app_frame(frame, (uint8_t)NB_LINK_CHANNEL_EVENT,
                              NB_LINK_MSG_TOUCH_EVENT, 1U, body, sizeof(body));
    frame[n - 1] ^= 0xFFU; /* corrompe payload */
    uint32_t before = nb_link_engine_stats(&fx.head_e)->dropped_invalid;
    nb_link_engine_on_frame(&fx.head_e, frame, n);

    TEST("corrupt_dropped",
         nb_link_engine_stats(&fx.head_e)->dropped_invalid == before + 1U);
    TEST("corrupt_not_delivered", fx.head_sink.count == 0);
}

static void test_peer_reboot_rehandshake(void)
{
    fixture_t fx;
    fixture_init(&fx, 100U, 200U);
    fixture_start(&fx);
    fixture_run(&fx, 250U, 4);
    TEST("ready_before_reboot", nb_link_engine_is_operational(&fx.head_e));

    /* Main reinicia com novo boot_id e refaz o handshake. */
    nb_link_engine_config_t main_cfg = {
        .role = NB_LINK_ROLE_MAIN,
        .boot_id = 101U,
        .version_major = 1U,
        .version_minor = 1U,
        .transport = {.send = sim_send, .ctx = &fx.ep_a},
        .on_message = on_message,
        .on_tx_result = on_tx_result,
        .user_ctx = &fx.main_sink,
    };
    nb_link_engine_init(&fx.main_e, &main_cfg);
    nb_link_engine_start(&fx.main_e, fx.clock);
    wire_step(&fx.wire, &fx.main_e, &fx.head_e);

    TEST("peer_reboot_detected",
         nb_link_engine_stats(&fx.head_e)->peer_reboots >= 1U);
    TEST("relinked_main", nb_link_engine_is_operational(&fx.main_e));
    TEST("relinked_head", nb_link_engine_is_operational(&fx.head_e));
}

static void test_version_major_mismatch(void)
{
    fixture_t fx;
    fixture_init(&fx, 1U, 2U);
    /* head com major incompatível */
    nb_link_engine_config_t head_cfg = {
        .role = NB_LINK_ROLE_HEAD,
        .boot_id = 2U,
        .version_major = 2U,
        .version_minor = 0U,
        .transport = {.send = sim_send, .ctx = &fx.ep_b},
        .on_message = on_message,
        .user_ctx = &fx.head_sink,
    };
    nb_link_engine_init(&fx.head_e, &head_cfg);
    fixture_start(&fx);
    fixture_run(&fx, 250U, 4);

    TEST("head_not_ready_on_major_mismatch",
         !nb_link_engine_is_operational(&fx.head_e));
    TEST("major_mismatch_dropped",
         nb_link_engine_stats(&fx.head_e)->dropped_invalid >= 1U);
}

int main(void)
{
    test_handshake();
    test_heartbeat_keeps_alive();
    test_snapshot_loss_retries_before_ready();
    test_timeout_then_recover();
    test_app_delivery_and_dedup();
    test_ack_latency_telemetry();
    test_lost_ack_retries_same_sequence();
    test_pending_aborted_on_peer_reboot();
    test_ack_timeout_is_explicit();
    test_control_preempts_bulk();
    test_bulk_backpressure();
    test_send_before_ready();
    test_corrupt_frame_dropped();
    test_peer_reboot_rehandshake();
    test_version_major_mismatch();

    printf("%d/%d testes passaram\n", s_pass, s_pass + s_fail);
    return s_fail == 0 ? 0 : 1;
}
