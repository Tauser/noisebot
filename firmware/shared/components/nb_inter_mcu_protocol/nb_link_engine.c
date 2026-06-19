/*
 * nb_link_engine — implementação da FSM do enlace inter-MCU (DM1, software).
 * C17 puro; transporte injetado. Ver nb_link_engine.h.
 */

#include "nb_link_engine.h"

#include <string.h>

#define NB_LINK_HEADER_BYTES (sizeof(nb_link_frame_header_t))

/* Ordem de prioridade na drenagem: controle/evento preemptam bulk. */
static const nb_link_channel_t k_tx_priority[NB_LINK_CHANNEL_COUNT] = {
    NB_LINK_CHANNEL_CONTROL,
    NB_LINK_CHANNEL_EVENT,
    NB_LINK_CHANNEL_STORAGE,
    NB_LINK_CHANNEL_DIAGNOSTIC,
    NB_LINK_CHANNEL_BULK,
};

static uint16_t interval_ms(const nb_link_engine_t *e)
{
    return e->cfg.heartbeat_interval_ms ? e->cfg.heartbeat_interval_ms
                                        : NB_LINK_HEARTBEAT_INTERVAL_MS;
}

static uint16_t timeout_ms(const nb_link_engine_t *e)
{
    return e->cfg.heartbeat_timeout_ms ? e->cfg.heartbeat_timeout_ms
                                       : NB_LINK_HEARTBEAT_TIMEOUT_MS;
}

static bool elapsed(uint32_t now, uint32_t since, uint32_t span)
{
    return (uint32_t)(now - since) >= span;
}

static void set_state(nb_link_engine_t *e, nb_link_state_t next)
{
    nb_link_state_t prev = e->state;
    if (prev == next) {
        return;
    }
    if (next == NB_LINK_STATE_READY) {
        ++e->stats.ready_transitions;
        e->last_hb_tx_ms = e->now_ms;
        e->last_hb_rx_ms = e->now_ms;
        if (prev == NB_LINK_STATE_SNAPSHOT ||
            prev == NB_LINK_STATE_HANDSHAKE) {
            const uint32_t duration_ms =
                (uint32_t)(e->now_ms - e->handshake_start_ms);
            e->stats.handshake_last_ms = duration_ms;
            if (duration_ms > e->stats.handshake_max_ms) {
                e->stats.handshake_max_ms = duration_ms;
            }
            ++e->stats.handshake_samples;
        }
    }
    if (prev == NB_LINK_STATE_READY) {
        ++e->stats.link_drops;
    }
    e->state = next;
    if (e->cfg.on_state_change) {
        e->cfg.on_state_change(e->cfg.user_ctx, prev, next);
    }
}

/* Constrói, finaliza e envia imediatamente um frame de controle/liveness. */
static bool emit_control(nb_link_engine_t *e,
                         nb_link_channel_t channel,
                         uint16_t message_type,
                         uint8_t flags,
                         const void *payload,
                         uint16_t length)
{
    if (e->cfg.transport.send == NULL || length > NB_LINK_ENGINE_SLOT_BYTES) {
        return false;
    }

    nb_link_frame_header_t header = {
        .magic = NB_LINK_MAGIC,
        .version_major = e->cfg.version_major,
        .version_minor = e->negotiated_minor,
        .channel = (uint8_t)channel,
        .flags = flags,
        .message_type = message_type,
        .sequence = e->tx_sequence[channel]++,
        .payload_length = length,
    };
    nb_link_frame_finalize(&header, payload);

    uint8_t buf[NB_LINK_HEADER_BYTES + NB_LINK_ENGINE_SLOT_BYTES];
    memcpy(buf, &header, NB_LINK_HEADER_BYTES);
    if (length > 0U && payload != NULL) {
        memcpy(buf + NB_LINK_HEADER_BYTES, payload, length);
    }

    bool ok = e->cfg.transport.send(e->cfg.transport.ctx, buf,
                                    NB_LINK_HEADER_BYTES + length);
    if (ok) {
        ++e->stats.frames_tx;
    }
    return ok;
}

static void fill_hello(const nb_link_engine_t *e, nb_link_hello_t *hello)
{
    hello->boot_id = e->cfg.boot_id;
    hello->uptime_ms = e->now_ms;
    hello->capability_bits = e->cfg.capability_bits;
    hello->max_payload_bytes = (uint16_t)NB_LINK_MAX_PAYLOAD_BYTES;
    hello->role = (uint8_t)e->cfg.role;
    hello->reserved = 0U;
}

static void send_hello(nb_link_engine_t *e, uint16_t type, uint8_t flags)
{
    nb_link_hello_t hello;
    fill_hello(e, &hello);
    (void)emit_control(e, NB_LINK_CHANNEL_CONTROL, type, flags, &hello,
                       (uint16_t)sizeof(hello));
}

static void send_snapshot(nb_link_engine_t *e, uint8_t flags)
{
    (void)emit_control(e, NB_LINK_CHANNEL_CONTROL,
                       NB_LINK_MSG_STATE_SNAPSHOT, flags, NULL, 0U);
}

static void send_heartbeat(nb_link_engine_t *e)
{
    if (emit_control(e, NB_LINK_CHANNEL_CONTROL, NB_LINK_MSG_HEARTBEAT, 0U,
                     NULL, 0U)) {
        ++e->stats.heartbeats_tx;
        e->last_hb_tx_ms = e->now_ms;
    }
}

static void complete_tx_slot(nb_link_engine_t *e,
                             nb_link_tx_slot_t *slot,
                             nb_link_tx_result_t result)
{
    if (e->cfg.on_tx_result != NULL) {
        e->cfg.on_tx_result(e->cfg.user_ctx, result,
                            (nb_link_channel_t)slot->channel,
                            slot->message_type, slot->sequence);
    }
    slot->used = false;
}

/* Atualiza o peer boot_id; reseta dedup e re-handshake quando muda. */
static void adopt_peer_boot_id(nb_link_engine_t *e, uint32_t boot_id)
{
    if (e->peer_boot_id_valid && e->peer_boot_id != boot_id) {
        ++e->stats.peer_reboots;
        for (unsigned i = 0; i < NB_LINK_ENGINE_TX_SLOTS; ++i) {
            if (e->tx[i].used && e->tx[i].ack_required) {
                complete_tx_slot(e, &e->tx[i],
                                 NB_LINK_TX_ABORTED_PEER_REBOOT);
                ++e->stats.aborted_on_reboot;
            }
        }
    }
    if (!e->peer_boot_id_valid || e->peer_boot_id != boot_id) {
        nb_link_sequence_tracker_reset(&e->rx_tracker);
    }
    e->peer_boot_id = boot_id;
    e->peer_boot_id_valid = true;
}

static bool version_major_ok(const nb_link_engine_t *e, uint8_t frame_major)
{
    return frame_major == e->cfg.version_major; /* major incompatível bloqueia */
}

/* minor negociado = menor entre local e peer (compatibilidade retroativa). */
static uint8_t negotiate_minor(const nb_link_engine_t *e, uint8_t peer_minor)
{
    return peer_minor < e->cfg.version_minor ? peer_minor : e->cfg.version_minor;
}

static int find_free_slot(const nb_link_engine_t *e)
{
    for (unsigned i = 0; i < NB_LINK_ENGINE_TX_SLOTS; ++i) {
        if (!e->tx[i].used) {
            return (int)i;
        }
    }
    return -1;
}

static bool slot_is_sendable(const nb_link_engine_t *e,
                             const nb_link_tx_slot_t *slot)
{
    if (!slot->used) {
        return false;
    }
    if (!slot->sent) {
        return true;
    }
    return slot->ack_required &&
           elapsed(e->now_ms, slot->last_tx_ms, NB_LINK_ACK_RETRY_MS);
}

static int find_best_slot(const nb_link_engine_t *e)
{
    for (unsigned p = 0; p < NB_LINK_CHANNEL_COUNT; ++p) {
        nb_link_channel_t channel = k_tx_priority[p];
        for (unsigned i = 0; i < NB_LINK_ENGINE_TX_SLOTS; ++i) {
            if (slot_is_sendable(e, &e->tx[i]) &&
                e->tx[i].channel == (uint8_t)channel) {
                return (int)i;
            }
        }
    }
    return -1;
}

static void pump_tx(nb_link_engine_t *e)
{
    if (e->cfg.transport.send == NULL ||
        e->state != NB_LINK_STATE_READY) {
        return;
    }
    for (unsigned guard = 0; guard < NB_LINK_ENGINE_TX_SLOTS; ++guard) {
        int idx = find_best_slot(e);
        if (idx < 0) {
            return;
        }
        nb_link_tx_slot_t *slot = &e->tx[idx];
        if (slot->sent && slot->retries >= NB_LINK_ACK_MAX_RETRIES) {
            complete_tx_slot(e, slot, NB_LINK_TX_TIMEOUT);
            ++e->stats.ack_timeouts;
            continue;
        }
        if (!e->cfg.transport.send(e->cfg.transport.ctx, slot->bytes,
                                   slot->length)) {
            return; /* transporte ocupado/queda: tenta no próximo tick */
        }
        if (slot->sent) {
            ++slot->retries;
            ++e->stats.ack_retries_tx;
        } else {
            slot->first_tx_ms = e->now_ms;
        }
        slot->sent = true;
        slot->last_tx_ms = e->now_ms;
        if (!slot->ack_required) {
            slot->used = false;
        }
        ++e->stats.frames_tx;
    }
}

void nb_link_engine_init(nb_link_engine_t *engine,
                         const nb_link_engine_config_t *config)
{
    memset(engine, 0, sizeof(*engine));
    engine->cfg = *config;
    engine->state = NB_LINK_STATE_RESET;
    engine->negotiated_minor = config->version_minor;
    nb_link_sequence_tracker_reset(&engine->rx_tracker);
    nb_link_credit_set(&engine->bulk_credits, 0U, 0U);
}

void nb_link_engine_start(nb_link_engine_t *engine, uint32_t now_ms)
{
    engine->now_ms = now_ms;
    engine->last_hb_rx_ms = now_ms;
    engine->last_hb_tx_ms = now_ms;
    engine->last_handshake_tx_ms = now_ms;
    engine->handshake_start_ms = now_ms;
    engine->handshake_retries = 0U;
    set_state(engine, NB_LINK_STATE_HANDSHAKE);
    if (engine->cfg.role == NB_LINK_ROLE_MAIN) {
        send_hello(engine, NB_LINK_MSG_HELLO, NB_LINK_FLAG_ACK_REQUIRED);
    }
}

void nb_link_engine_tick(nb_link_engine_t *engine, uint32_t now_ms)
{
    engine->now_ms = now_ms;

    if ((engine->state == NB_LINK_STATE_HANDSHAKE ||
         engine->state == NB_LINK_STATE_SNAPSHOT) &&
        engine->cfg.role == NB_LINK_ROLE_MAIN) {
        if (engine->handshake_retries < NB_LINK_HANDSHAKE_MAX_RETRIES &&
            elapsed(now_ms, engine->last_handshake_tx_ms,
                    NB_LINK_HANDSHAKE_RETRY_MS)) {
            if (engine->state == NB_LINK_STATE_HANDSHAKE) {
                send_hello(engine, NB_LINK_MSG_HELLO,
                           NB_LINK_FLAG_ACK_REQUIRED);
            } else {
                send_snapshot(engine, NB_LINK_FLAG_ACK_REQUIRED |
                                      NB_LINK_FLAG_RETRY);
            }
            engine->last_handshake_tx_ms = now_ms;
            ++engine->handshake_retries;
        }
    }

    if (engine->state == NB_LINK_STATE_READY ||
        engine->state == NB_LINK_STATE_DEGRADED) {
        if (elapsed(now_ms, engine->last_hb_tx_ms, interval_ms(engine))) {
            send_heartbeat(engine);
        }
        if (engine->state == NB_LINK_STATE_READY &&
            elapsed(now_ms, engine->last_hb_rx_ms, timeout_ms(engine))) {
            set_state(engine, NB_LINK_STATE_DEGRADED);
        }
    }

    pump_tx(engine);
}

static void handle_control(nb_link_engine_t *e,
                           const nb_link_frame_header_t *header,
                           const void *payload)
{
    switch (header->message_type) {
    case NB_LINK_MSG_HELLO: {
        if (e->cfg.role != NB_LINK_ROLE_HEAD ||
            header->payload_length < sizeof(nb_link_hello_t)) {
            ++e->stats.dropped_invalid;
            return;
        }
        nb_link_hello_t hello;
        memcpy(&hello, payload, sizeof(hello));
        if (!version_major_ok(e, header->version_major)) {
            ++e->stats.dropped_invalid;
            return;
        }
        e->negotiated_minor = negotiate_minor(e, header->version_minor);
        adopt_peer_boot_id(e, hello.boot_id);
        e->peer_capability_bits = hello.capability_bits;
        send_hello(e, NB_LINK_MSG_HELLO_ACK, NB_LINK_FLAG_RESPONSE);
        set_state(e, NB_LINK_STATE_SNAPSHOT);
        break;
    }
    case NB_LINK_MSG_HELLO_ACK: {
        if (e->cfg.role != NB_LINK_ROLE_MAIN ||
            header->payload_length < sizeof(nb_link_hello_t)) {
            ++e->stats.dropped_invalid;
            return;
        }
        nb_link_hello_t hello;
        memcpy(&hello, payload, sizeof(hello));
        if (!version_major_ok(e, header->version_major)) {
            ++e->stats.dropped_invalid;
            return;
        }
        e->negotiated_minor = negotiate_minor(e, header->version_minor);
        adopt_peer_boot_id(e, hello.boot_id);
        e->peer_capability_bits = hello.capability_bits;
        e->handshake_retries = 0U;
        e->last_handshake_tx_ms = e->now_ms;
        set_state(e, NB_LINK_STATE_SNAPSHOT);
        send_snapshot(e, NB_LINK_FLAG_ACK_REQUIRED);
        break;
    }
    case NB_LINK_MSG_STATE_SNAPSHOT:
        if (e->cfg.role == NB_LINK_ROLE_HEAD &&
            (header->flags & NB_LINK_FLAG_RESPONSE) == 0U &&
            (e->state == NB_LINK_STATE_SNAPSHOT ||
             e->state == NB_LINK_STATE_HANDSHAKE ||
             e->state == NB_LINK_STATE_READY)) {
            send_snapshot(e, NB_LINK_FLAG_RESPONSE);
            set_state(e, NB_LINK_STATE_READY);
        } else if (e->cfg.role == NB_LINK_ROLE_MAIN &&
                   (header->flags & NB_LINK_FLAG_RESPONSE) != 0U &&
                   e->state == NB_LINK_STATE_SNAPSHOT) {
            set_state(e, NB_LINK_STATE_READY);
        } else {
            ++e->stats.dropped_invalid;
        }
        break;
    case NB_LINK_MSG_HEARTBEAT:
        ++e->stats.heartbeats_rx;
        if (e->state == NB_LINK_STATE_DEGRADED) {
            set_state(e, NB_LINK_STATE_READY);
        }
        break;
    case NB_LINK_MSG_CREDIT_UPDATE:
        if (header->payload_length >= sizeof(nb_link_credit_update_t)) {
            nb_link_credit_update_t credit;
            memcpy(&credit, payload, sizeof(credit));
            nb_link_credit_set(&e->bulk_credits, credit.frame_credits,
                               credit.byte_credits);
        }
        break;
    case NB_LINK_MSG_TIME_SYNC:
        /* DM1-software: aceito como liveness; offset aplicado em fase posterior. */
        break;
    case NB_LINK_MSG_ACK:
        if (header->payload_length >= sizeof(nb_link_ack_t)) {
            nb_link_ack_t ack;
            memcpy(&ack, payload, sizeof(ack));
            for (unsigned i = 0; i < NB_LINK_ENGINE_TX_SLOTS; ++i) {
                nb_link_tx_slot_t *slot = &e->tx[i];
                if (slot->used && slot->ack_required &&
                    slot->channel == ack.channel &&
                    slot->message_type == ack.message_type &&
                    slot->sequence == ack.sequence) {
                    const uint32_t rtt_ms =
                        (uint32_t)(e->now_ms - slot->last_tx_ms);
                    const uint32_t e2e_ms =
                        (uint32_t)(e->now_ms - slot->first_tx_ms);
                    e->stats.ack_rtt_last_ms = rtt_ms;
                    e->stats.ack_rtt_total_ms += rtt_ms;
                    if (rtt_ms > e->stats.ack_rtt_max_ms) {
                        e->stats.ack_rtt_max_ms = rtt_ms;
                    }
                    e->stats.ack_e2e_last_ms = e2e_ms;
                    if (e2e_ms > e->stats.ack_e2e_max_ms) {
                        e->stats.ack_e2e_max_ms = e2e_ms;
                    }
                    ++e->stats.ack_rtt_samples;
                    complete_tx_slot(e, slot, NB_LINK_TX_ACKED);
                    ++e->stats.acked_tx;
                    return;
                }
            }
            ++e->stats.dropped_stale;
        } else {
            ++e->stats.dropped_invalid;
        }
        break;
    default:
        ++e->stats.dropped_invalid;
        break;
    }
}

static bool is_control_message(uint16_t type)
{
    return type < (uint16_t)NB_LINK_MSG_DISPLAY_COMMAND;
}

static void send_ack(nb_link_engine_t *e,
                     const nb_link_frame_header_t *received)
{
    nb_link_ack_t ack = {
        .sequence = received->sequence,
        .message_type = received->message_type,
        .channel = received->channel,
        .reserved = 0U,
    };
    (void)emit_control(e, NB_LINK_CHANNEL_CONTROL, NB_LINK_MSG_ACK,
                       NB_LINK_FLAG_RESPONSE, &ack, (uint16_t)sizeof(ack));
}

void nb_link_engine_on_frame(nb_link_engine_t *engine,
                             const void *frame,
                             size_t length)
{
    if (nb_link_frame_validate(frame, length) != NB_LINK_VALIDATE_OK) {
        ++engine->stats.dropped_invalid;
        return;
    }

    nb_link_frame_header_t header;
    memcpy(&header, frame, NB_LINK_HEADER_BYTES);
    const void *payload = (const uint8_t *)frame + NB_LINK_HEADER_BYTES;

    ++engine->stats.frames_rx;
    engine->last_hb_rx_ms = engine->now_ms; /* qualquer frame válido = peer vivo */

    if (is_control_message(header.message_type)) {
        handle_control(engine, &header, payload);
        return;
    }

    /* Dados de aplicação só após o link subir e com peer conhecido. */
    if (engine->state != NB_LINK_STATE_READY || !engine->peer_boot_id_valid) {
        ++engine->stats.dropped_invalid;
        return;
    }

    nb_link_sequence_result_t seq = nb_link_sequence_track(
        &engine->rx_tracker, engine->peer_boot_id,
        (nb_link_channel_t)header.channel, header.sequence);

    if (seq == NB_LINK_SEQUENCE_RETRY) {
        ++engine->stats.retries_rx;
        if ((header.flags & NB_LINK_FLAG_ACK_REQUIRED) != 0U) {
            send_ack(engine, &header);
        }
        return;
    }
    if (seq == NB_LINK_SEQUENCE_STALE) {
        ++engine->stats.dropped_stale;
        return;
    }

    if (engine->cfg.on_message) {
        engine->cfg.on_message(engine->cfg.user_ctx,
                               (nb_link_channel_t)header.channel,
                               header.message_type, payload,
                               (uint16_t)header.payload_length);
    }
    if ((header.flags & NB_LINK_FLAG_ACK_REQUIRED) != 0U) {
        send_ack(engine, &header);
    }
}

bool nb_link_engine_send(nb_link_engine_t *engine,
                         nb_link_channel_t channel,
                         uint16_t message_type,
                         const void *payload,
                         uint16_t length)
{
    if (channel >= NB_LINK_CHANNEL_COUNT || length > NB_LINK_ENGINE_SLOT_BYTES) {
        return false;
    }
    if (engine->state != NB_LINK_STATE_READY &&
        engine->state != NB_LINK_STATE_DEGRADED) {
        ++engine->stats.tx_backpressure;
        return false;
    }

    bool credit_taken = false;
    if (channel == NB_LINK_CHANNEL_BULK) {
        if (!nb_link_credit_try_consume(&engine->bulk_credits, length)) {
            ++engine->stats.tx_backpressure;
            return false;
        }
        credit_taken = true;
    }

    int idx = find_free_slot(engine);
    if (idx < 0) {
        if (credit_taken) {
            nb_link_credit_release(&engine->bulk_credits, length);
        }
        ++engine->stats.tx_backpressure;
        return false;
    }

    nb_link_tx_slot_t *slot = &engine->tx[idx];
    nb_link_frame_header_t header = {
        .magic = NB_LINK_MAGIC,
        .version_major = engine->cfg.version_major,
        .version_minor = engine->negotiated_minor,
        .channel = (uint8_t)channel,
        .flags = NB_LINK_FLAG_ACK_REQUIRED,
        .message_type = message_type,
        .sequence = engine->tx_sequence[channel]++,
        .payload_length = length,
    };
    nb_link_frame_finalize(&header, payload);

    memcpy(slot->bytes, &header, NB_LINK_HEADER_BYTES);
    if (length > 0U && payload != NULL) {
        memcpy(slot->bytes + NB_LINK_HEADER_BYTES, payload, length);
    }
    slot->channel = (uint8_t)channel;
    slot->message_type = message_type;
    slot->sequence = header.sequence;
    slot->length = (uint16_t)(NB_LINK_HEADER_BYTES + length);
    slot->sent = false;
    slot->ack_required = true;
    slot->retries = 0U;
    slot->first_tx_ms = 0U;
    slot->last_tx_ms = 0U;
    slot->used = true;

    pump_tx(engine);
    return true;
}

nb_link_state_t nb_link_engine_state(const nb_link_engine_t *engine)
{
    return engine->state;
}

bool nb_link_engine_is_operational(const nb_link_engine_t *engine)
{
    return engine->state == NB_LINK_STATE_READY;
}

const nb_link_engine_stats_t *nb_link_engine_stats(const nb_link_engine_t *engine)
{
    return &engine->stats;
}

bool nb_link_engine_peer_has_capability(const nb_link_engine_t *engine,
                                        uint32_t capability)
{
    return engine != NULL && engine->peer_boot_id_valid &&
           capability != 0U &&
           (engine->peer_capability_bits & capability) == capability;
}

void nb_link_engine_set_bulk_credits(nb_link_engine_t *engine,
                                     uint16_t frame_credits,
                                     uint32_t byte_credits)
{
    nb_link_credit_set(&engine->bulk_credits, frame_credits, byte_credits);
}
