/*
 * bridge_service.h — Bridge LLM Protocol Service (Layer 2, Etapa 12.1)
 *
 * Conecta o ESP32 a um bridge LLM externo (RPi/PC na mesma rede) via TCP ou
 * UART (USB CDC). Offline-first: o robot opera normalmente sem bridge ativo.
 *
 * Framing (idêntico em ambos os transportes):
 *   [0xAB][LEN_LO][LEN_HI][TYPE][DATA...][CRC8]
 *   - SOF  : 1 byte  (0xAB)
 *   - LEN  : 2 bytes little-endian = número de bytes de DATA (sem TYPE)
 *   - TYPE : 1 byte  (nb_bridge_msg_type_t)
 *   - DATA : LEN bytes
 *   - CRC8 : 1 byte, CRC-8/SMBUS sobre (TYPE + DATA)
 *
 * Transporte TCP:
 *   ESP32 age como servidor TCP na porta NB_BRIDGE_TCP_PORT (9000).
 *   Keep-alive: detecta queda em < 10s.
 *   Reconexão: aceita nova conexão do bridge a cada 5s por 60s após queda.
 *   Após 60s sem reconexão, permanece offline até próximo boot.
 *
 * Transporte UART (fallback de desenvolvimento):
 *   USB CDC nativo do ESP32-S3 (usb_serial_jtag), 921600 baud equivalente.
 *   Requer CONFIG_ESP_CONSOLE_UART_DEFAULT=UART0 em menuconfig para separar
 *   o console de debug da interface bridge.
 *
 * Seleção de transporte no boot:
 *   WiFi com IP → aguarda TCP por 2s → timeout → tenta UART handshake (200ms)
 *   Sem WiFi   → tenta UART handshake (200ms) diretamente
 *   Sem resposta em ambos → modo OFFLINE (operação normal sem bridge)
 *
 * Mensagens ESP32 → Bridge:
 *   AUDIO_CHUNK: int16_t pcm[256] (256 samples × 2 bytes = 512 bytes DATA)
 *   EVENT      : uint32_t type + uint8_t data[8] (12 bytes DATA)
 *   STATUS     : state(1) + valence(4) + activation(4) + attention(4) + health(1) = 14 bytes
 *
 * Mensagens Bridge → ESP32 (publicadas via event bus):
 *   SAY        → NB_EVT_BRIDGE_SAY       (data.ptr = nb_bridge_say_chunk_t*)
 *   EXPR       → NB_EVT_BRIDGE_EXPR      (data.ptr = nb_bridge_expr_cmd_t*)
 *   ACTION     → NB_EVT_BRIDGE_ACTION    (data.u32 = nb_bridge_action_t)
 *   EMOT_EVENT → NB_EVT_BRIDGE_EMOT_EVENT(data.u32 = nb_emotion_event_t)
 *   GAZE       → NB_EVT_BRIDGE_GAZE      (data.bytes = float x,y)
 *   TEXT_SCROLL→ NB_EVT_BRIDGE_TEXT_SCROLL(data.ptr = static char[129])
 *   VOLUME     → NB_EVT_BRIDGE_VOLUME    (data.u32 = 0..100)
 *
 * Thread-safety: todas as funções públicas são thread-safe via mutex.
 *
 * Task: "nb_bridge_task"  Core 0  Prio 4  Stack 4KB
 */

#ifndef NB_BRIDGE_SERVICE_H
#define NB_BRIDGE_SERVICE_H

#include "esp_err.h"
#include "nb_events.h"
#include "event_bus.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Configuração ─────────────────────────────────────────────────────────── */

#define NB_BRIDGE_TASK_STACK        4096U
#define NB_BRIDGE_TASK_PRIORITY     4U
#define NB_BRIDGE_TASK_CORE         0

#define NB_BRIDGE_TCP_PORT          9000
#define NB_BRIDGE_TCP_LISTEN_MS     2000    /* espera por cliente TCP no boot  */
#define NB_BRIDGE_UART_HANDSHAKE_MS 200     /* timeout handshake UART          */
#define NB_BRIDGE_KEEPALIVE_IDLE_S  5       /* TCP keep-alive idle             */
#define NB_BRIDGE_KEEPALIVE_INT_S   2       /* TCP keep-alive intervalo        */
#define NB_BRIDGE_KEEPALIVE_CNT     3       /* TCP keep-alive retries          */
#define NB_BRIDGE_RECONNECT_INTVL_MS 5000   /* intervalo entre tentativas TCP  */
#define NB_BRIDGE_RECONNECT_TIMEOUT_MS 60000/* para de tentar após 60s         */

#define NB_BRIDGE_AUDIO_CHUNK_SAMPLES 256   /* amostras por chunk PCM          */
#define NB_BRIDGE_TEXT_MAX_LEN      128     /* tamanho máximo TEXT_SCROLL      */

/* ── Tipos de mensagem ────────────────────────────────────────────────────── */

typedef enum {
    /* ESP32 → Bridge */
    NB_BRIDGE_MSG_AUDIO_CHUNK  = 0x01,   /* PCM int16[], 256 samples          */
    NB_BRIDGE_MSG_EVENT        = 0x02,   /* nb_event_type_t + data[8]         */
    NB_BRIDGE_MSG_STATUS       = 0x03,   /* estado geral do robot             */
    NB_BRIDGE_MSG_SESSION      = 0x04,   /* evento de sessão v2 em JSON       */
    /* Bridge → ESP32 */
    NB_BRIDGE_MSG_SAY          = 0x10,   /* chunk PCM int16[], 256 samples    */
    NB_BRIDGE_MSG_EXPR         = 0x11,   /* expressão + duração               */
    NB_BRIDGE_MSG_ACTION       = 0x12,   /* ação pré-definida                 */
    NB_BRIDGE_MSG_EMOT_EVENT   = 0x13,   /* nb_emotion_event_t                */
    NB_BRIDGE_MSG_GAZE         = 0x14,   /* float x, float y [-1..1]          */
    NB_BRIDGE_MSG_TEXT_SCROLL  = 0x15,   /* string UTF-8, máx 128 bytes       */
    NB_BRIDGE_MSG_VOLUME       = 0x16,   /* volume 0..100                     */
    /* Handshake */
    NB_BRIDGE_MSG_HELLO        = 0x00,   /* handshake hello (ambas direções)  */
} nb_bridge_msg_type_t;

/* ── Transporte ───────────────────────────────────────────────────────────── */

typedef enum {
    NB_BRIDGE_TRANSPORT_OFFLINE = 0,
    NB_BRIDGE_TRANSPORT_TCP     = 1,
    NB_BRIDGE_TRANSPORT_UART    = 2,
} nb_bridge_transport_t;

/* ── Ações pré-definidas (ACTION) ─────────────────────────────────────────── */

typedef enum {
    NB_BRIDGE_ACTION_GREET      = 0,
    NB_BRIDGE_ACTION_NOD        = 1,
    NB_BRIDGE_ACTION_SHAKE      = 2,
    NB_BRIDGE_ACTION_LOOK_UP    = 3,
    NB_BRIDGE_ACTION_LOOK_DOWN  = 4,
    NB_BRIDGE_ACTION_COUNT      = 5,
} nb_bridge_action_t;

/* ── Structs de payload para eventos publicados ────────────────────────────── */

/** Payload de NB_EVT_BRIDGE_SAY — ponteiro válido apenas durante handler síncrono. */
typedef struct {
    int16_t  samples[NB_BRIDGE_AUDIO_CHUNK_SAMPLES];
    uint16_t count;
} nb_bridge_say_chunk_t;

/** Payload de NB_EVT_BRIDGE_EXPR — ponteiro válido apenas durante handler síncrono. */
typedef struct {
    uint8_t  expression_id;   /* mapeado para nb_expression_t */
    uint32_t duration_ms;
} nb_bridge_expr_cmd_t;

/* ── Status enviado ao bridge ─────────────────────────────────────────────── */

typedef struct {
    uint8_t  state;           /* nb_robot_state_t */
    float    valence;         /* [-1..1] */
    float    activation;      /* [-1..1] */
    float    attention;       /* [0..1]  */
    uint8_t  health_score;    /* 0..100  */
} nb_bridge_status_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o bridge service e inicia a task.
 *
 * Deve ser chamado em PHASE_SERVICES após wifi_service_init().
 * Retorna ESP_OK imediatamente; a seleção de transporte ocorre na task.
 */
esp_err_t bridge_service_init(void);

/**
 * @brief Retorna o transporte ativo atual.
 * Thread-safe.
 */
nb_bridge_transport_t bridge_service_get_transport(void);

/**
 * @brief Retorna true se o bridge está conectado (TCP ou UART).
 * Thread-safe.
 */
bool bridge_service_is_connected(void);

/**
 * @brief Limpa a fila de transmissão do bridge.
 *
 * Útil antes de iniciar uma nova sessão de voz: garante que VOICE_START e os
 * primeiros chunks não sejam bloqueados por frames antigos.
 */
void bridge_service_flush_tx(void);

/**
 * @brief Envia chunk de áudio PCM para o bridge.
 *
 * Chamado pela camada de áudio (Layer 4) durante VAD VOICE_ACTIVE.
 * Descartado silenciosamente se bridge offline ou buffer de TX cheio.
 *
 * @param samples  Buffer PCM int16 (NB_BRIDGE_AUDIO_CHUNK_SAMPLES amostras).
 * @param count    Número de amostras (máx NB_BRIDGE_AUDIO_CHUNK_SAMPLES).
 * @return ESP_OK se enfileirado, ESP_ERR_INVALID_STATE se offline.
 */
esp_err_t bridge_service_send_audio_chunk(const int16_t *samples, uint16_t count);

/**
 * @brief Envia evento do event bus para o bridge.
 *
 * @param evt  Evento a encaminhar.
 * @return ESP_OK se enfileirado, ESP_ERR_INVALID_STATE se offline.
 */
esp_err_t bridge_service_send_event(const nb_event_t *evt);

/**
 * @brief Atualiza o status a ser enviado periodicamente ao bridge.
 *
 * Chamado por camadas superiores (conductor/behavior) que têm acesso
 * aos getters de state_machine, emotion_model, attention_service, etc.
 * Thread-safe.
 *
 * @param status  Status atual do robot.
 */
void bridge_service_update_status(const nb_bridge_status_t *status);

/** Versão do protocolo negociada no handshake (1 ou 2; 0 = nunca conectou). Thread-safe. */
uint8_t bridge_service_get_protocol_version(void);

/** Tempo desde o último frame recebido do bridge (ms). UINT32_MAX se nunca conectou. Thread-safe. */
uint32_t bridge_service_get_last_rx_age_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_BRIDGE_SERVICE_H */
