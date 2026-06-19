#ifndef NB_LINK_ENGINE_H
#define NB_LINK_ENGINE_H

/*
 * nb_link_engine — máquina de estado do enlace inter-MCU (DM1, software).
 *
 * C17 puro, sem ESP-IDF nem FreeRTOS: testável no host com transporte
 * simulado. O firmware envolve este motor numa task da Layer 2 e liga
 * `transport.send` ao SPI master/slave; nada aqui inicializa GPIO.
 *
 * Papéis assimétricos no handshake (ver DUAL_MCU_ARCHITECTURE_PLAN seção 5):
 *   MAIN envia HELLO → recebe HELLO_ACK → envia STATE_SNAPSHOT → READY.
 *   HEAD aguarda HELLO → envia HELLO_ACK → recebe STATE_SNAPSHOT → READY.
 *
 * O motor não chama behavior/HAL: entrega eventos por callback para a
 * Layer 4 republicar no event bus.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nb_inter_mcu_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NB_LINK_HEARTBEAT_INTERVAL_MS 250U
#define NB_LINK_HEARTBEAT_TIMEOUT_MS  1000U
#define NB_LINK_HANDSHAKE_RETRY_MS    250U
#define NB_LINK_HANDSHAKE_MAX_RETRIES 16U
#define NB_LINK_ACK_RETRY_MS          100U
#define NB_LINK_ACK_MAX_RETRIES       5U

#define NB_LINK_ENGINE_TX_SLOTS   16U
#define NB_LINK_ENGINE_SLOT_BYTES 64U

typedef enum {
    NB_LINK_ROLE_MAIN = 0,
    NB_LINK_ROLE_HEAD,
} nb_link_role_t;

typedef enum {
    NB_LINK_STATE_RESET = 0, /* nada negociado / baseline desconectado */
    NB_LINK_STATE_HANDSHAKE, /* HELLO em curso                         */
    NB_LINK_STATE_SNAPSHOT,  /* handshake ok, snapshot pendente        */
    NB_LINK_STATE_READY,     /* operacional                            */
    NB_LINK_STATE_DEGRADED,  /* heartbeat perdido após READY           */
} nb_link_state_t;

typedef bool (*nb_link_transport_send_fn)(void *ctx,
                                          const void *frame,
                                          size_t length);

typedef struct {
    nb_link_transport_send_fn send;
    void *ctx;
} nb_link_transport_t;

typedef void (*nb_link_state_cb_fn)(void *ctx,
                                    nb_link_state_t previous,
                                    nb_link_state_t current);

typedef void (*nb_link_message_cb_fn)(void *ctx,
                                      nb_link_channel_t channel,
                                      uint16_t message_type,
                                      const void *payload,
                                      uint16_t length);

typedef enum {
    NB_LINK_TX_ACKED = 0,
    NB_LINK_TX_TIMEOUT,
    NB_LINK_TX_ABORTED_PEER_REBOOT,
} nb_link_tx_result_t;

typedef void (*nb_link_tx_result_cb_fn)(void *ctx,
                                        nb_link_tx_result_t result,
                                        nb_link_channel_t channel,
                                        uint16_t message_type,
                                        uint32_t sequence);

typedef struct {
    nb_link_role_t role;
    uint32_t boot_id;
    uint8_t version_major;
    uint8_t version_minor;
    uint32_t capability_bits;
    uint16_t heartbeat_interval_ms; /* 0 → usa default */
    uint16_t heartbeat_timeout_ms;  /* 0 → usa default */
    nb_link_transport_t transport;
    nb_link_state_cb_fn on_state_change; /* opcional */
    nb_link_message_cb_fn on_message;    /* opcional */
    nb_link_tx_result_cb_fn on_tx_result; /* opcional */
    void *user_ctx;
} nb_link_engine_config_t;

typedef struct {
    uint32_t frames_tx;
    uint32_t frames_rx;
    uint32_t dropped_invalid; /* CRC/magic/versão/canal/truncamento */
    uint32_t dropped_stale;   /* sequence antiga                    */
    uint32_t retries_rx;      /* duplicado (mesma sequence)         */
    uint32_t tx_backpressure; /* enqueue rejeitado (fila/créditos)  */
    uint32_t ack_retries_tx;  /* retransmissões com mesma sequence  */
    uint32_t ack_timeouts;    /* sem ACK após limite                */
    uint32_t acked_tx;        /* operações confirmadas pelo peer    */
    uint32_t aborted_on_reboot; /* pendências não repetidas no reboot */
    uint32_t heartbeats_tx;
    uint32_t heartbeats_rx;
    uint32_t ready_transitions; /* quantas vezes chegou a READY     */
    uint32_t link_drops;        /* READY → DEGRADED/RESET           */
    uint32_t peer_reboots;      /* novo boot_id do peer detectado   */
    uint32_t handshake_last_ms; /* HANDSHAKE → primeiro READY       */
    uint32_t handshake_max_ms;
    uint32_t handshake_samples;
    uint32_t ack_rtt_last_ms;   /* RTT desde a tentativa confirmada */
    uint32_t ack_rtt_max_ms;
    uint64_t ack_rtt_total_ms;
    uint32_t ack_rtt_samples;
    uint32_t ack_e2e_last_ms;   /* envio inicial → ACK, inclui retry */
    uint32_t ack_e2e_max_ms;
} nb_link_engine_stats_t;

typedef struct {
    bool used;
    bool sent;
    bool ack_required;
    uint8_t channel;
    uint8_t retries;
    uint16_t message_type;
    uint16_t length; /* header + payload */
    uint32_t sequence;
    uint32_t first_tx_ms;
    uint32_t last_tx_ms;
    uint8_t bytes[sizeof(nb_link_frame_header_t) + NB_LINK_ENGINE_SLOT_BYTES];
} nb_link_tx_slot_t;

typedef struct {
    nb_link_engine_config_t cfg;
    nb_link_state_t state;
    uint32_t now_ms;
    uint32_t last_hb_tx_ms;
    uint32_t last_hb_rx_ms;
    uint32_t last_handshake_tx_ms;
    uint32_t handshake_start_ms;
    uint8_t handshake_retries;
    bool peer_boot_id_valid;
    uint32_t peer_boot_id;
    uint8_t negotiated_minor;
    uint32_t tx_sequence[NB_LINK_CHANNEL_COUNT];
    nb_link_sequence_tracker_t rx_tracker;
    nb_link_credit_state_t bulk_credits;
    nb_link_tx_slot_t tx[NB_LINK_ENGINE_TX_SLOTS];
    nb_link_engine_stats_t stats;
} nb_link_engine_t;

/* Inicializa o motor; estado inicial RESET, sem emitir frame. */
void nb_link_engine_init(nb_link_engine_t *engine,
                         const nb_link_engine_config_t *config);

/* Inicia o handshake: MAIN emite HELLO; HEAD passa a aguardar. */
void nb_link_engine_start(nb_link_engine_t *engine, uint32_t now_ms);

/* Avança o relógio (ms monotônico injetado): heartbeat, timeout, retransmissão
 * de handshake e drenagem das filas. Determinístico para teste. */
void nb_link_engine_tick(nb_link_engine_t *engine, uint32_t now_ms);

/* Alimenta um frame recebido do transporte. Valida, roda a FSM e, para dados
 * de aplicação novos, chama `on_message`. */
void nb_link_engine_on_frame(nb_link_engine_t *engine,
                             const void *frame,
                             size_t length);

/* Enfileira um frame de aplicação. BULK consome créditos; retorna false em
 * backpressure (fila cheia ou créditos insuficientes). */
bool nb_link_engine_send(nb_link_engine_t *engine,
                         nb_link_channel_t channel,
                         uint16_t message_type,
                         const void *payload,
                         uint16_t length);

nb_link_state_t nb_link_engine_state(const nb_link_engine_t *engine);
bool nb_link_engine_is_operational(const nb_link_engine_t *engine);
const nb_link_engine_stats_t *nb_link_engine_stats(const nb_link_engine_t *engine);

/* Define a janela de créditos do canal BULK (reabastecida via CREDIT_UPDATE). */
void nb_link_engine_set_bulk_credits(nb_link_engine_t *engine,
                                     uint16_t frame_credits,
                                     uint32_t byte_credits);

#ifdef __cplusplus
}
#endif

#endif /* NB_LINK_ENGINE_H */
