/*
 * bridge_service.c — Bridge LLM Protocol Service (Etapa 12.1)
 */

#include "bridge_service.h"
#include "logger.h"
#include "event_bus.h"
#include "wifi_service.h"
#include "nb_events.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/usb_serial_jtag.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/err.h"
#include "lwip/tcp.h"

#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>

#define TAG "nb_bridge"

/* ── Frame ────────────────────────────────────────────────────────────────── */

#define FRAME_SOF        0xABu
#define FRAME_SOF_OFF    0
#define FRAME_LEN_LO_OFF 1
#define FRAME_LEN_HI_OFF 2
#define FRAME_TYPE_OFF   3
#define FRAME_DATA_OFF   4
/* CRC segue imediatamente após DATA */

/* overhead fixo: SOF(1) + LEN(2) + TYPE(1) + CRC(1) = 5 bytes */
#define FRAME_OVERHEAD   5u

/* tamanho máximo de frame: maior payload é AUDIO_CHUNK = 256×2 = 512 bytes */
#define FRAME_MAX_PAYLOAD  (NB_BRIDGE_AUDIO_CHUNK_SAMPLES * 2u)
#define FRAME_MAX_SIZE     (FRAME_OVERHEAD + FRAME_MAX_PAYLOAD)

static const char BRIDGE_HELLO_V2[] =
    "{\"protocol\":\"noisebot-bridge\",\"version\":2,\"role\":\"firmware\","
    "\"audio\":{\"format\":\"pcm16\",\"sample_rate\":16000,\"channels\":1,"
    "\"chunk_samples\":256},\"rx\":[\"say\",\"expr\",\"action\","
    "\"emot_event\",\"gaze\",\"text_scroll\",\"volume\",\"hello\",\"session\"],"
    "\"tx\":[\"audio_chunk\",\"event\",\"status\",\"hello\",\"session\"],"
    "\"features\":[\"voice_events\",\"status\",\"device_commands_v1\",\"volume_control\","
    "\"session_events_v2\"]}";

/* ── TX queue ─────────────────────────────────────────────────────────────── */

/* Cada item na fila é um frame serializado completo precedido por tamanho */
#define TX_QUEUE_DEPTH  128u /* PSRAM: absorve bursts de WiFi sem cortar fala */
#define TX_ITEM_SIZE    (sizeof(uint16_t) + FRAME_MAX_SIZE)
#define TX_AUDIO_BACKLOG_MAX  124u /* reserva espaço para eventos de controle */

typedef struct {
    uint16_t len;
    uint8_t  data[FRAME_MAX_SIZE];
} tx_item_t;

/* ── Estado interno ──────────────────────────────────────────────────────── */

typedef enum {
    BS_STATE_INIT        = 0,
    BS_STATE_SELECTING,
    BS_STATE_TCP_WAIT,
    BS_STATE_CONNECTED,
    BS_STATE_RECONNECTING,
    BS_STATE_OFFLINE,
} bridge_state_t;

static struct {
    bridge_state_t          state;
    nb_bridge_transport_t   transport;
    nb_bridge_status_t      status;
    SemaphoreHandle_t       mutex;
    QueueHandle_t           tx_queue;

    /* TCP */
    int                     server_fd;
    int                     client_fd;

    /* UART (USB CDC) */
    bool                    uart_installed;

    /* Reconexão */
    int64_t                 disconnect_time_us;

    /* Diagnóstico */
    uint8_t                 proto_version;   /* versão negociada no handshake */
    int64_t                 last_rx_time_us; /* timestamp do último frame recebido */
} s;

/* USB CDC desabilitado por padrão: GPIO 19/20 são usados pelo servo HAL (UART1).
 * Habilitar apenas se GPIO 19/20 estiverem livres de periféricos concorrentes. */
static bool s_usb_cdc_enabled = false;

/* ── CRC-8/SMBUS (poly 0x07, init 0x00) ──────────────────────────────────── */

static uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00u;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1u) ^ 0x07u) : (uint8_t)(crc << 1u);
        }
    }
    return crc;
}

/* ── Frame encode ─────────────────────────────────────────────────────────── */

/* Serializa mensagem em buf. Retorna tamanho total ou 0 se payload > máximo. */
static uint16_t frame_encode(uint8_t *buf, nb_bridge_msg_type_t type,
                              const void *payload, uint16_t payload_len)
{
    if (payload_len > FRAME_MAX_PAYLOAD) {
        return 0;
    }

    buf[FRAME_SOF_OFF]    = FRAME_SOF;
    buf[FRAME_LEN_LO_OFF] = (uint8_t)(payload_len & 0xFFu);
    buf[FRAME_LEN_HI_OFF] = (uint8_t)((payload_len >> 8u) & 0xFFu);
    buf[FRAME_TYPE_OFF]   = (uint8_t)type;

    if (payload_len > 0u && payload != NULL) {
        memcpy(&buf[FRAME_DATA_OFF], payload, payload_len);
    }

    /* CRC cobre TYPE + DATA */
    uint8_t crc_src[1u + FRAME_MAX_PAYLOAD];
    crc_src[0] = (uint8_t)type;
    if (payload_len > 0u && payload != NULL) {
        memcpy(&crc_src[1], payload, payload_len);
    }
    buf[FRAME_DATA_OFF + payload_len] = crc8(crc_src, 1u + payload_len);

    return (uint16_t)(FRAME_OVERHEAD + payload_len);
}

/* ── Frame decode result ──────────────────────────────────────────────────── */

typedef enum {
    DECODE_OK        = 0,
    DECODE_SOF_ERR,
    DECODE_CRC_ERR,
    DECODE_LEN_ERR,
} decode_result_t;

static decode_result_t frame_decode(const uint8_t *buf, uint16_t buf_len,
                                    nb_bridge_msg_type_t *out_type,
                                    const uint8_t **out_data,
                                    uint16_t *out_data_len)
{
    if (buf_len < FRAME_OVERHEAD) return DECODE_LEN_ERR;
    if (buf[FRAME_SOF_OFF] != FRAME_SOF) return DECODE_SOF_ERR;

    uint16_t data_len = (uint16_t)(buf[FRAME_LEN_LO_OFF]) |
                        (uint16_t)(buf[FRAME_LEN_HI_OFF] << 8u);

    if ((uint16_t)(FRAME_OVERHEAD + data_len) > buf_len) return DECODE_LEN_ERR;
    if (data_len > FRAME_MAX_PAYLOAD) return DECODE_LEN_ERR;

    uint8_t crc_src[1u + FRAME_MAX_PAYLOAD];
    crc_src[0] = buf[FRAME_TYPE_OFF];
    if (data_len > 0u) {
        memcpy(&crc_src[1], &buf[FRAME_DATA_OFF], data_len);
    }
    uint8_t expected_crc = crc8(crc_src, 1u + data_len);
    uint8_t received_crc = buf[FRAME_DATA_OFF + data_len];

    if (expected_crc != received_crc) return DECODE_CRC_ERR;

    *out_type     = (nb_bridge_msg_type_t)buf[FRAME_TYPE_OFF];
    *out_data     = &buf[FRAME_DATA_OFF];
    *out_data_len = data_len;
    return DECODE_OK;
}

/* ── Buffers estáticos para eventos publicados ─────────────────────────────
 * Usados com nb_event_publish (síncrono): lifetime cobre a chamada. */

static nb_bridge_say_chunk_t  s_say_buf;
static nb_bridge_expr_cmd_t   s_expr_buf;
static char                   s_text_buf[NB_BRIDGE_TEXT_MAX_LEN + 1u];
static char                   s_session_buf[97u];
static StaticQueue_t          s_tx_queue_static;
static uint8_t               *s_tx_queue_storage;

static bool     s_diag_waiting_first_audio = false;
static uint32_t s_diag_audio_chunks = 0u;
static uint32_t s_diag_audio_drops = 0u;
static bool     s_diag_audio_drop_logged = false;
static volatile uint32_t s_tx_flush_seq = 0u;

typedef enum {
    TCP_SEND_OK = 0,
    TCP_SEND_WOULD_BLOCK,
    TCP_SEND_FATAL,
} tcp_send_result_t;

static void tx_queue_flush(void)
{
    if (s.tx_queue) {
        xQueueReset(s.tx_queue);
    }
    s_tx_flush_seq++;
}

static esp_err_t enqueue_frame(nb_bridge_msg_type_t type,
                               const void *payload, uint16_t payload_len);

/* ── Dispatch mensagem recebida do bridge ─────────────────────────────────── */

static void dispatch_incoming(nb_bridge_msg_type_t type,
                               const uint8_t *data, uint16_t data_len)
{
    int64_t now_us = esp_timer_get_time();
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    s.last_rx_time_us = now_us;
    xSemaphoreGive(s.mutex);

    nb_event_t evt = { .timestamp_ms = (uint32_t)(now_us / 1000) };

    switch (type) {

    case NB_BRIDGE_MSG_HELLO:
        if (data_len > 0u) {
            /* Detecta versão anunciada pelo bridge no payload JSON. */
            if (data_len >= 2u) {
                const char *ver = (const char *)data;
                if (strstr(ver, "\"version\":2") || strstr(ver, "\"version\": 2")) {
                    xSemaphoreTake(s.mutex, portMAX_DELAY);
                    s.proto_version = 2;
                    xSemaphoreGive(s.mutex);
                } else {
                    xSemaphoreTake(s.mutex, portMAX_DELAY);
                    s.proto_version = 1;
                    xSemaphoreGive(s.mutex);
                }
            }
            esp_err_t err = enqueue_frame(NB_BRIDGE_MSG_HELLO,
                                          BRIDGE_HELLO_V2,
                                          (uint16_t)strlen(BRIDGE_HELLO_V2));
            if (err != ESP_OK) {
                NB_LOGW(TAG, "HELLO v2 resposta falhou: %s", esp_err_to_name(err));
            } else {
                NB_LOGI(TAG, "HELLO v2 recebido — capabilities enviadas (proto_v=%u)",
                        (unsigned)s.proto_version);
            }
        }
        break;

    case NB_BRIDGE_MSG_SESSION:
        {
            uint16_t copy_len = (data_len < (sizeof(s_session_buf) - 1u))
                                ? data_len : (uint16_t)(sizeof(s_session_buf) - 1u);
            if (copy_len > 0u) {
                memcpy(s_session_buf, data, copy_len);
            }
            s_session_buf[copy_len] = '\0';
            NB_LOGI(TAG, "SESSION v2 recebido len=%u payload=%s%s",
                    data_len, s_session_buf,
                    (data_len > copy_len) ? "..." : "");
            evt.type     = NB_EVT_BRIDGE_SESSION;
            evt.data.ptr = s_session_buf;
            nb_event_publish(&evt);
        }
        break;

    case NB_BRIDGE_MSG_SAY:
        if (data_len > sizeof(s_say_buf.samples)) break;
        s_say_buf.count = (uint16_t)(data_len / 2u);
        if (data_len > 0u) {
            memcpy(s_say_buf.samples, data, data_len);
        }
        evt.type    = NB_EVT_BRIDGE_SAY;
        evt.data.ptr = &s_say_buf;
        nb_event_publish(&evt);
        break;

    case NB_BRIDGE_MSG_EXPR:
        if (data_len < 5u) break;
        s_expr_buf.expression_id = data[0];
        s_expr_buf.duration_ms   = (uint32_t)data[1]
                                 | ((uint32_t)data[2] << 8u)
                                 | ((uint32_t)data[3] << 16u)
                                 | ((uint32_t)data[4] << 24u);
        evt.type     = NB_EVT_BRIDGE_EXPR;
        evt.data.ptr = &s_expr_buf;
        nb_event_publish(&evt);
        break;

    case NB_BRIDGE_MSG_ACTION:
        if (data_len < 4u) break;
        evt.type    = NB_EVT_BRIDGE_ACTION;
        evt.data.u32 = (uint32_t)data[0]
                     | ((uint32_t)data[1] << 8u)
                     | ((uint32_t)data[2] << 16u)
                     | ((uint32_t)data[3] << 24u);
        nb_event_publish(&evt);
        break;

    case NB_BRIDGE_MSG_EMOT_EVENT:
        if (data_len < 1u) break;
        evt.type     = NB_EVT_BRIDGE_EMOT_EVENT;
        evt.data.u32 = data[0];
        nb_event_publish(&evt);
        break;

    case NB_BRIDGE_MSG_GAZE:
        if (data_len < 8u) break;
        evt.type = NB_EVT_BRIDGE_GAZE;
        memcpy(evt.data.bytes, data, 8u);   /* float x then float y */
        nb_event_publish(&evt);
        break;

    case NB_BRIDGE_MSG_TEXT_SCROLL:
        if (data_len == 0u) break;
        {
            uint16_t copy_len = (data_len < NB_BRIDGE_TEXT_MAX_LEN)
                                ? data_len : NB_BRIDGE_TEXT_MAX_LEN;
            memcpy(s_text_buf, data, copy_len);
            s_text_buf[copy_len] = '\0';
            evt.type     = NB_EVT_BRIDGE_TEXT_SCROLL;
            evt.data.ptr = s_text_buf;
            nb_event_publish(&evt);
        }
        break;

    case NB_BRIDGE_MSG_VOLUME:
        if (data_len < 1u) break;
        evt.type     = NB_EVT_BRIDGE_VOLUME;
        evt.data.u32 = (data[0] > 100u) ? 100u : data[0];
        nb_event_publish(&evt);
        break;

    default:
        NB_LOGW(TAG, "msg type 0x%02X desconhecida (len=%u)", (unsigned)type, data_len);
        break;
    }
}

/* ── Envio genérico via TX queue ──────────────────────────────────────────── */

static esp_err_t enqueue_frame_ex(nb_bridge_msg_type_t type,
                                  const void *payload, uint16_t payload_len,
                                  bool front)
{
    tx_item_t item;
    item.len = frame_encode(item.data, type, payload, payload_len);
    if (item.len == 0u) return ESP_ERR_INVALID_SIZE;

    BaseType_t ok = front
                  ? xQueueSendToFront(s.tx_queue, &item, 0)
                  : xQueueSend(s.tx_queue, &item, 0);
    if (ok != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t enqueue_frame(nb_bridge_msg_type_t type,
                               const void *payload, uint16_t payload_len)
{
    return enqueue_frame_ex(type, payload, payload_len, false);
}

static void tcp_configure_client(int fd)
{
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    struct timeval io_timeout = {
        .tv_sec = 0,
        .tv_usec = 100000,
    };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout));
}

/* ── TCP helpers ──────────────────────────────────────────────────────────── */

static int tcp_create_server(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* TCP keep-alive: detecta queda em < 10s */
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    int ka_idle = NB_BRIDGE_KEEPALIVE_IDLE_S;
    int ka_int  = NB_BRIDGE_KEEPALIVE_INT_S;
    int ka_cnt  = NB_BRIDGE_KEEPALIVE_CNT;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &ka_idle, sizeof(ka_idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &ka_int,  sizeof(ka_int));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &ka_cnt,  sizeof(ka_cnt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(NB_BRIDGE_TCP_PORT),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Aceita cliente com timeout_ms. Retorna fd do cliente ou -1. */
static int tcp_accept_timeout(int server_fd, uint32_t timeout_ms)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(server_fd, &rfds);
    struct timeval tv = {
        .tv_sec  = (long)(timeout_ms / 1000u),
        .tv_usec = (long)((timeout_ms % 1000u) * 1000u),
    };
    int ret = select(server_fd + 1, &rfds, NULL, NULL, &tv);
    if (ret <= 0) return -1;

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    return accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
}

/* Envia buffer via TCP mantendo progresso parcial. */
static tcp_send_result_t tcp_send_progress(int fd,
                                           const uint8_t *data,
                                           uint16_t len,
                                           uint16_t *offset)
{
    while (*offset < len) {
        int sent = send(fd, data + *offset, (size_t)(len - *offset), 0);
        if (sent <= 0) {
            int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK || err == EINPROGRESS) {
                return TCP_SEND_WOULD_BLOCK;
            }
            NB_LOGW(TAG, "TCP send falhou sent=%u/%u errno=%d",
                    (unsigned)*offset, (unsigned)len, err);
            return TCP_SEND_FATAL;
        }
        *offset = (uint16_t)(*offset + (uint16_t)sent);
    }
    return TCP_SEND_OK;
}

static bool tcp_send_all(int fd, const uint8_t *data, uint16_t len)
{
    uint16_t offset = 0u;
    int retries = 0;
    while (offset < len) {
        tcp_send_result_t res = tcp_send_progress(fd, data, len, &offset);
        if (res == TCP_SEND_OK) return true;
        if (res == TCP_SEND_FATAL) return false;
        if (++retries >= 10) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

/* Recebe até max_len bytes com timeout. Retorna bytes lidos ou -1 em erro. */
static int tcp_recv_timeout(int fd, uint8_t *buf, int max_len, uint32_t timeout_ms)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = {
        .tv_sec  = (long)(timeout_ms / 1000u),
        .tv_usec = (long)((timeout_ms % 1000u) * 1000u),
    };
    int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (ret <= 0) return (ret == 0) ? 0 : -1;

    int received = recv(fd, buf, max_len, 0);
    if (received < 0) {
        int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK || err == EINPROGRESS) {
            return 0;
        }
    }
    return (received == 0) ? -1 : received;
}

/* ── UART (USB CDC) helpers ───────────────────────────────────────────────── */

static esp_err_t uart_install(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = FRAME_MAX_SIZE * 2u,
        .tx_buffer_size = FRAME_MAX_SIZE * 2u,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err == ESP_OK) {
        s.uart_installed = true;
    }
    return err;
}

static bool uart_send(const uint8_t *data, uint16_t len)
{
    uint16_t written_total = 0u;
    while (written_total < len) {
        int written = usb_serial_jtag_write_bytes(data + written_total,
                                                  (uint32_t)(len - written_total),
                                                  pdMS_TO_TICKS(100));
        if (written <= 0) {
            return false;
        }
        written_total = (uint16_t)(written_total + (uint16_t)written);
    }
    return true;
}

static int uart_recv(uint8_t *buf, uint32_t max_len, uint32_t timeout_ms)
{
    return usb_serial_jtag_read_bytes(buf, max_len, pdMS_TO_TICKS(timeout_ms));
}

/* ── Handshake genérico ───────────────────────────────────────────────────── */

/* Envia HELLO e aguarda HELLO de volta. Retorna true se handshake OK. */
static bool do_handshake_tcp(int client_fd, uint32_t timeout_ms)
{
    uint8_t frame[FRAME_OVERHEAD];
    uint16_t len = frame_encode(frame, NB_BRIDGE_MSG_HELLO, NULL, 0u);
    if (!tcp_send_all(client_fd, frame, len)) return false;

    uint8_t rx[FRAME_OVERHEAD];
    int n = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    while (n < (int)FRAME_OVERHEAD) {
        int64_t remaining_ms = (deadline - esp_timer_get_time()) / 1000LL;
        if (remaining_ms <= 0) break;
        int r = tcp_recv_timeout(client_fd, rx + n, (int)FRAME_OVERHEAD - n,
                                 (uint32_t)remaining_ms + 1u);
        if (r <= 0) return false;
        n += r;
    }
    if (n < (int)FRAME_OVERHEAD) return false;

    nb_bridge_msg_type_t t;
    const uint8_t *d;
    uint16_t dl;
    return (frame_decode(rx, (uint16_t)n, &t, &d, &dl) == DECODE_OK &&
            t == NB_BRIDGE_MSG_HELLO);
}

static bool do_handshake_uart(uint32_t timeout_ms)
{
    uint8_t frame[FRAME_OVERHEAD];
    uint16_t len = frame_encode(frame, NB_BRIDGE_MSG_HELLO, NULL, 0u);
    if (!uart_send(frame, len)) return false;

    uint8_t rx[FRAME_OVERHEAD];
    int n = uart_recv(rx, (uint32_t)FRAME_OVERHEAD, timeout_ms);
    if (n < (int)FRAME_OVERHEAD) return false;

    nb_bridge_msg_type_t t;
    const uint8_t *d;
    uint16_t dl;
    return (frame_decode(rx, (uint16_t)n, &t, &d, &dl) == DECODE_OK &&
            t == NB_BRIDGE_MSG_HELLO);
}

/* ── Publicar evento de conexão ───────────────────────────────────────────── */

static void publish_connected(nb_bridge_transport_t transport)
{
    tx_queue_flush();
    s_diag_waiting_first_audio = false;
    s_diag_audio_chunks = 0u;
    s_diag_audio_drops = 0u;
    s_diag_audio_drop_logged = false;

    xSemaphoreTake(s.mutex, portMAX_DELAY);
    s.transport = transport;
    xSemaphoreGive(s.mutex);

    nb_event_t evt = {
        .type         = NB_EVT_BRIDGE_CONNECTED,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
        .data.u32     = (uint32_t)transport,
    };
    nb_event_publish_async(&evt);

    const char *tname = (transport == NB_BRIDGE_TRANSPORT_TCP) ? "TCP" : "UART";
    NB_LOGI(TAG, "bridge conectado via %s", tname);
}

static void publish_disconnected(void)
{
    tx_queue_flush();
    s_diag_waiting_first_audio = false;
    s_diag_audio_chunks = 0u;
    s_diag_audio_drops = 0u;
    s_diag_audio_drop_logged = false;

    xSemaphoreTake(s.mutex, portMAX_DELAY);
    s.transport = NB_BRIDGE_TRANSPORT_OFFLINE;
    xSemaphoreGive(s.mutex);

    nb_event_t evt = {
        .type         = NB_EVT_BRIDGE_DISCONNECTED,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
    };
    nb_event_publish_async(&evt);
    NB_LOGW(TAG, "bridge desconectado — tentando reconectar");
}

/* ── Loop de I/O TCP ──────────────────────────────────────────────────────── */

/* RX ring buffer para remontar frames fragmentados */
static uint8_t s_rx_buf[FRAME_MAX_SIZE * 2u];
static uint16_t s_rx_head = 0u;

static void tcp_io_loop(void)
{
    s_rx_head = 0u;
    tx_item_t tx_item;
    bool tx_pending = false;
    uint16_t tx_offset = 0u;
    uint32_t seen_flush_seq = s_tx_flush_seq;

    /* STATUS periódico a cada 5s */
    int64_t next_status_us = esp_timer_get_time() + 5000000LL;

    while (true) {
        if (seen_flush_seq != s_tx_flush_seq) {
            tx_pending = false;
            tx_offset = 0u;
            seen_flush_seq = s_tx_flush_seq;
        }

        /* TX: drenar fila */
        while (true) {
            if (!tx_pending) {
                if (xQueueReceive(s.tx_queue, &tx_item, 0) != pdTRUE) {
                    break;
                }
                tx_pending = true;
                tx_offset = 0u;
            }

            tcp_send_result_t send_res = tcp_send_progress(s.client_fd,
                                                           tx_item.data,
                                                           tx_item.len,
                                                           &tx_offset);
            if (send_res == TCP_SEND_OK) {
                tx_pending = false;
                tx_offset = 0u;
                continue;
            }
            if (send_res == TCP_SEND_WOULD_BLOCK) {
                NB_LOGD(TAG, "TCP send sem espaço — mantendo frame pendente");
                break;
            }
            NB_LOGD(TAG, "TCP send fatal — desconectando");
            goto tcp_disconnect;
        }

        /* STATUS periódico */
        if (!tx_pending && esp_timer_get_time() >= next_status_us) {
            xSemaphoreTake(s.mutex, portMAX_DELAY);
            nb_bridge_status_t status_snap = s.status;
            xSemaphoreGive(s.mutex);

            uint8_t payload[14];
            payload[0] = status_snap.state;
            memcpy(&payload[1],  &status_snap.valence,     4u);
            memcpy(&payload[5],  &status_snap.activation,  4u);
            memcpy(&payload[9],  &status_snap.attention,   4u);
            payload[13] = status_snap.health_score;

            uint8_t frame[FRAME_OVERHEAD + 14u];
            uint16_t flen = frame_encode(frame, NB_BRIDGE_MSG_STATUS, payload, 14u);
            if (!tcp_send_all(s.client_fd, frame, flen)) {
                NB_LOGD(TAG, "TCP status send falhou — desconectando");
                goto tcp_disconnect;
            }
            next_status_us = esp_timer_get_time() + 5000000LL;
        }

        /* RX: lê dados disponíveis */
        uint32_t rx_wait_ms = tx_pending ? 5u : 10u;
        int n = tcp_recv_timeout(s.client_fd,
                                 s_rx_buf + s_rx_head,
                                 (int)(sizeof(s_rx_buf) - s_rx_head),
                                 rx_wait_ms);
        if (n < 0) {
            NB_LOGD(TAG, "TCP recv erro — desconectando");
            goto tcp_disconnect;
        }
        if (n > 0) {
            s_rx_head = (uint16_t)(s_rx_head + (uint16_t)n);
        }

        /* Parse frames completos do RX buffer */
        uint16_t consumed = 0u;
        while (consumed + FRAME_OVERHEAD <= s_rx_head) {
            uint8_t *p = s_rx_buf + consumed;

            /* Buscar SOF */
            if (p[FRAME_SOF_OFF] != FRAME_SOF) {
                consumed++;
                continue;
            }

            uint16_t data_len = (uint16_t)p[FRAME_LEN_LO_OFF] |
                                 (uint16_t)(p[FRAME_LEN_HI_OFF] << 8u);
            uint16_t total    = (uint16_t)(FRAME_OVERHEAD + data_len);

            if (consumed + total > s_rx_head) break;   /* frame incompleto */

            nb_bridge_msg_type_t type;
            const uint8_t *fdata;
            uint16_t fdata_len;
            decode_result_t res = frame_decode(p, total, &type, &fdata, &fdata_len);
            if (res == DECODE_OK) {
                dispatch_incoming(type, fdata, fdata_len);
            } else if (res == DECODE_CRC_ERR) {
                NB_LOGW(TAG, "CRC error (type=0x%02X len=%u) — frame descartado",
                        (unsigned)p[FRAME_TYPE_OFF], data_len);
            }
            consumed = (uint16_t)(consumed + total);
        }

        /* Compact RX buffer */
        if (consumed > 0u && consumed <= s_rx_head) {
            s_rx_head = (uint16_t)(s_rx_head - consumed);
            if (s_rx_head > 0u) {
                memmove(s_rx_buf, s_rx_buf + consumed, s_rx_head);
            }
        }
    }

tcp_disconnect:
    close(s.client_fd);
    s.client_fd = -1;
}

/* ── Loop de I/O UART ─────────────────────────────────────────────────────── */

static void uart_io_loop(void)
{
    s_rx_head = 0u;
    tx_item_t tx_item;
    int64_t next_status_us = esp_timer_get_time() + 5000000LL;

    while (true) {
        /* TX */
        while (xQueueReceive(s.tx_queue, &tx_item, 0) == pdTRUE) {
            uart_send(tx_item.data, tx_item.len);
        }

        /* STATUS periódico */
        if (esp_timer_get_time() >= next_status_us) {
            xSemaphoreTake(s.mutex, portMAX_DELAY);
            nb_bridge_status_t status_snap = s.status;
            xSemaphoreGive(s.mutex);

            uint8_t payload[14];
            payload[0] = status_snap.state;
            memcpy(&payload[1],  &status_snap.valence,    4u);
            memcpy(&payload[5],  &status_snap.activation, 4u);
            memcpy(&payload[9],  &status_snap.attention,  4u);
            payload[13] = status_snap.health_score;

            uint8_t frame[FRAME_OVERHEAD + 14u];
            uint16_t flen = frame_encode(frame, NB_BRIDGE_MSG_STATUS, payload, 14u);
            uart_send(frame, flen);
            next_status_us = esp_timer_get_time() + 5000000LL;
        }

        /* RX */
        int n = uart_recv(s_rx_buf + s_rx_head,
                          (uint32_t)(sizeof(s_rx_buf) - s_rx_head),
                          50u);
        if (n > 0) {
            s_rx_head = (uint16_t)(s_rx_head + (uint16_t)n);
        }

        /* Parse frames */
        uint16_t consumed = 0u;
        while (consumed + FRAME_OVERHEAD <= s_rx_head) {
            uint8_t *p = s_rx_buf + consumed;
            if (p[FRAME_SOF_OFF] != FRAME_SOF) { consumed++; continue; }

            uint16_t data_len = (uint16_t)p[FRAME_LEN_LO_OFF] |
                                 (uint16_t)(p[FRAME_LEN_HI_OFF] << 8u);
            uint16_t total    = (uint16_t)(FRAME_OVERHEAD + data_len);
            if (consumed + total > s_rx_head) break;

            nb_bridge_msg_type_t type;
            const uint8_t *fdata;
            uint16_t fdata_len;
            decode_result_t res = frame_decode(p, total, &type, &fdata, &fdata_len);
            if (res == DECODE_OK) {
                dispatch_incoming(type, fdata, fdata_len);
            } else if (res == DECODE_CRC_ERR) {
                NB_LOGW(TAG, "UART CRC error — frame descartado");
            }
            consumed = (uint16_t)(consumed + total);
        }

        if (consumed > 0u && consumed <= s_rx_head) {
            s_rx_head = (uint16_t)(s_rx_head - consumed);
            if (s_rx_head > 0u) {
                memmove(s_rx_buf, s_rx_buf + consumed, s_rx_head);
            }
        }
    }
}

/* ── Task principal ───────────────────────────────────────────────────────── */

static void nb_bridge_task(void *arg)
{
    (void)arg;

    /* Instalar UART (USB CDC) apenas se habilitado.
     * Desabilitado por padrão: usb_serial_jtag_driver_install() toma GPIO 19/20
     * (USB D+/D-) no nível de hardware, conflitando com UART1 (servos). */
    if (s_usb_cdc_enabled && uart_install() != ESP_OK) {
        NB_LOGW(TAG, "USB CDC não disponível — UART fallback desabilitado");
    }

    /* ── Aguardar WiFi (até 30s) antes de abrir servidor TCP ─────────────── */
    for (int i = 0; i < 60 && !wifi_service_is_connected(); i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    bool wifi_up = wifi_service_is_connected();

    /* ── Tentar UART primeiro enquanto WiFi não está disponível ──────────── */
    if (!wifi_up && s.uart_installed) {
        NB_LOGI(TAG, "Sem WiFi — tentando handshake UART (USB CDC)...");
        if (do_handshake_uart(NB_BRIDGE_UART_HANDSHAKE_MS)) {
            publish_connected(NB_BRIDGE_TRANSPORT_UART);
            s.state = BS_STATE_CONNECTED;
            uart_io_loop();
            goto offline;
        }
    }

    /* ── Servidor TCP: fica ativo indefinidamente aceitando conexões ─────── */
    if (wifi_up) {
        NB_LOGI(TAG, "Servidor TCP porta %d aguardando bridge...", NB_BRIDGE_TCP_PORT);
        s.server_fd = tcp_create_server();
        if (s.server_fd < 0) {
            NB_LOGE(TAG, "tcp_create_server falhou");
            goto offline;
        }
    } else {
        NB_LOGI(TAG, "Sem WiFi e sem UART — modo OFFLINE");
        goto offline;
    }

    /* Loop principal: aceita, conecta, reconecta indefinidamente */
    while (true) {
        s.state = BS_STATE_TCP_WAIT;
        NB_LOGD(TAG, "Aguardando cliente TCP...");

        /* Aceita com poll de 1s para não bloquear indefinidamente */
        s.client_fd = -1;
        while (s.client_fd < 0) {
            s.client_fd = tcp_accept_timeout(s.server_fd, 1000u);
            /* Se WiFi caiu, aguarda reconexão */
            if (!wifi_service_is_connected()) {
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        }

        if (!do_handshake_tcp(s.client_fd, 300u)) {
            NB_LOGD(TAG, "TCP handshake falhou — aguardando próximo cliente");
            close(s.client_fd);
            s.client_fd = -1;
            continue;
        }

        tcp_configure_client(s.client_fd);
        publish_connected(NB_BRIDGE_TRANSPORT_TCP);
        s.state = BS_STATE_CONNECTED;
        tcp_io_loop();   /* bloqueia até desconexão */
        publish_disconnected();

        /* Após desconexão: volta a aguardar novo cliente imediatamente */
        NB_LOGI(TAG, "Bridge desconectado — aguardando reconexão");
    }

offline:
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    s.transport = NB_BRIDGE_TRANSPORT_OFFLINE;
    s.state     = BS_STATE_OFFLINE;
    xSemaphoreGive(s.mutex);
    NB_LOGI(TAG, "bridge_task encerrada — modo OFFLINE");
    vTaskDelete(NULL);
}

/* ── API pública ──────────────────────────────────────────────────────────── */

void bridge_service_enable_usb_cdc(void)
{
    s_usb_cdc_enabled = true;
}

esp_err_t bridge_service_init(void)
{
    s.mutex = xSemaphoreCreateMutex();
    if (!s.mutex) return ESP_ERR_NO_MEM;

    s_tx_queue_storage = (uint8_t *)heap_caps_malloc(TX_QUEUE_DEPTH * sizeof(tx_item_t),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_tx_queue_storage) {
        s.tx_queue = xQueueCreateStatic(TX_QUEUE_DEPTH,
                                        sizeof(tx_item_t),
                                        s_tx_queue_storage,
                                        &s_tx_queue_static);
        NB_LOGI(TAG, "fila TX alocada em PSRAM (%u bytes)",
                (unsigned)(TX_QUEUE_DEPTH * sizeof(tx_item_t)));
    } else {
        NB_LOGW(TAG, "PSRAM indisponivel para fila TX — usando SRAM interna");
        s.tx_queue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(tx_item_t));
    }
    if (!s.tx_queue) return ESP_ERR_NO_MEM;

    s.server_fd  = -1;
    s.client_fd  = -1;
    s.transport  = NB_BRIDGE_TRANSPORT_OFFLINE;
    s.state      = BS_STATE_INIT;
    s.uart_installed = false;

    BaseType_t ret = xTaskCreatePinnedToCore(
        nb_bridge_task, "nb_bridge_task",
        NB_BRIDGE_TASK_STACK, NULL,
        NB_BRIDGE_TASK_PRIORITY, NULL,
        NB_BRIDGE_TASK_CORE);

    if (ret != pdPASS) {
        NB_LOGE(TAG, "xTaskCreatePinnedToCore falhou");
        return ESP_ERR_NO_MEM;
    }

    NB_LOGI(TAG, "bridge_service iniciado");
    return ESP_OK;
}

nb_bridge_transport_t bridge_service_get_transport(void)
{
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    nb_bridge_transport_t t = s.transport;
    xSemaphoreGive(s.mutex);
    return t;
}

bool bridge_service_is_connected(void)
{
    return bridge_service_get_transport() != NB_BRIDGE_TRANSPORT_OFFLINE;
}

void bridge_service_flush_tx(void)
{
    tx_queue_flush();
}

esp_err_t bridge_service_send_audio_chunk(const int16_t *samples, uint16_t count)
{
    if (!bridge_service_is_connected()) return ESP_ERR_INVALID_STATE;
    if (count > NB_BRIDGE_AUDIO_CHUNK_SAMPLES) count = NB_BRIDGE_AUDIO_CHUNK_SAMPLES;

    if (s.tx_queue && uxQueueMessagesWaiting(s.tx_queue) >= TX_AUDIO_BACKLOG_MAX) {
        s_diag_audio_drops++;
        if (!s_diag_audio_drop_logged || (s_diag_audio_drops % 32u) == 0u) {
            s_diag_audio_drop_logged = true;
            NB_LOGW(TAG, "AUDIO_CHUNK dropado por backlog TX drops=%lu",
                    (unsigned long)s_diag_audio_drops);
        }
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = enqueue_frame(NB_BRIDGE_MSG_AUDIO_CHUNK, samples, (uint16_t)(count * 2u));
    if (err == ESP_OK) {
        s_diag_audio_chunks++;
        if (s_diag_waiting_first_audio) {
            s_diag_waiting_first_audio = false;
            NB_LOGI(TAG, "AUDIO_CHUNK primeiro enfileirado samples=%u bytes=%u",
                    (unsigned)count, (unsigned)(count * 2u));
        }
    } else {
        s_diag_audio_drops++;
        if (!s_diag_audio_drop_logged || (s_diag_audio_drops % 32u) == 0u) {
            s_diag_audio_drop_logged = true;
            NB_LOGW(TAG, "AUDIO_CHUNK nao entrou na fila: %s drops=%lu",
                    esp_err_to_name(err), (unsigned long)s_diag_audio_drops);
        }
    }
    return err;
}

esp_err_t bridge_service_send_event(const nb_event_t *evt)
{
    if (!evt) return ESP_ERR_INVALID_ARG;
    if (!bridge_service_is_connected()) return ESP_ERR_INVALID_STATE;

    uint8_t payload[12];
    uint32_t type_u = (uint32_t)evt->type;
    memcpy(&payload[0], &type_u, 4u);
    memcpy(&payload[4], evt->data.bytes, 8u);

    if (evt->type == NB_EVT_VOICE_ACTIVITY_START) {
        tx_queue_flush();
    }

    bool priority = (evt->type == NB_EVT_VOICE_ACTIVITY_START);
    esp_err_t err = enqueue_frame_ex(NB_BRIDGE_MSG_EVENT, payload, 12u, priority);
    if (err == ESP_ERR_NO_MEM && evt->type == NB_EVT_VOICE_ACTIVITY_END) {
        NB_LOGW(TAG, "fila TX cheia no VOICE_END — descartando áudio pendente");
        tx_queue_flush();
        err = enqueue_frame_ex(NB_BRIDGE_MSG_EVENT, payload, 12u, true);
    }
    if (err != ESP_OK) {
        NB_LOGW(TAG, "EVENT %lu nao entrou na fila: %s",
                (unsigned long)type_u, esp_err_to_name(err));
        return err;
    }

    if (evt->type == NB_EVT_VOICE_ACTIVITY_START) {
        s_diag_waiting_first_audio = true;
        s_diag_audio_chunks = 0u;
        s_diag_audio_drops = 0u;
        s_diag_audio_drop_logged = false;
        NB_LOGI(TAG, "VOICE_START enfileirado para bridge");
    } else if (evt->type == NB_EVT_VOICE_ACTIVITY_END) {
        NB_LOGI(TAG, "VOICE_END enfileirado para bridge chunks=%lu drops=%lu first_audio=%u",
                (unsigned long)s_diag_audio_chunks,
                (unsigned long)s_diag_audio_drops,
                s_diag_waiting_first_audio ? 0u : 1u);
        s_diag_waiting_first_audio = false;
    }
    return ESP_OK;
}

void bridge_service_update_status(const nb_bridge_status_t *status)
{
    if (!status) return;
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    s.status = *status;
    xSemaphoreGive(s.mutex);
}

uint8_t bridge_service_get_protocol_version(void)
{
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    uint8_t v = s.proto_version;
    xSemaphoreGive(s.mutex);
    return v;
}

uint32_t bridge_service_get_last_rx_age_ms(void)
{
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    int64_t last = s.last_rx_time_us;
    xSemaphoreGive(s.mutex);
    if (last == 0) return UINT32_MAX;
    int64_t age_ms = (esp_timer_get_time() - last) / 1000LL;
    return (age_ms < 0) ? 0u : (uint32_t)age_ms;
}
