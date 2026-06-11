/*
 * servo_hal.h — Driver UART para servos Feetech SCS0009 via FE-TTLinker
 *
 * Protocolo: SCSCL (pacote binário com header 0xFF 0xFF, checksum ~OR).
 * Etapa 3.1: apenas leitura e ping. Nenhum comando de movimento é emitido.
 *
 * UART1: TX=GPIO20, RX=GPIO19, 1Mbps.
 * IDs:   PAN=1 (pescoço esquerda/direita), TILT=2 (inclinação).
 *
 * Task: sem task própria — funções síncronas chamadas pelo boot_manager ou
 * motion_service (Etapa 3.2+). Retry máx 3 em timeout antes de retornar erro.
 */

#ifndef NB_SERVO_HAL_H
#define NB_SERVO_HAL_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Registradores de leitura do SCS0009 ─────────────────────────────────── */

/* ENDIANNESS: SCS0009 usa big-endian — byte ALTO no endereço menor.
 * Os sufixos "L"/"H" são os nomes da documentação Feetech (endereço menor/maior),
 * NÃO indicam byte baixo/alto. Confirmado via dump de registradores (maio 2026). */
#define NB_SERVO_REG_PRESENT_POS_L   0x38u   /* doc: "L" = endereço menor = H byte */
#define NB_SERVO_REG_PRESENT_POS_H   0x39u   /* doc: "H" = endereço maior = L byte */
#define NB_SERVO_REG_PRESENT_SPD_L   0x3Au   /* velocidade: endereço menor = H byte */
#define NB_SERVO_REG_PRESENT_SPD_H   0x3Bu   /* velocidade: endereço maior = L byte */
#define NB_SERVO_REG_PRESENT_LOAD_L  0x3Cu   /* carga: endereço menor = H byte      */
#define NB_SERVO_REG_PRESENT_LOAD_H  0x3Du   /* carga: endereço maior = L byte      */
#define NB_SERVO_REG_PRESENT_VOLTAGE 0x3Eu   /* voltagem em décimos de V */
#define NB_SERVO_REG_PRESENT_TEMP    0x3Fu   /* temperatura em °C        */

/* ── Parâmetros de comunicação ───────────────────────────────────────────── */

#define NB_SERVO_RETRY_MAX           3       /* tentativas antes de retornar erro */
#define NB_SERVO_TIMEOUT_MS          100     /* timeout de resposta por tentativa */

/* ── API pública ─────────────────────────────────────────────────────────── */

/**
 * servo_hal_init() — Inicializa UART1 para comunicação com os servos.
 *
 * Configura UART1 com os pinos e baud rate definidos em nb_hw_config.h.
 * Deve ser chamado uma vez durante o boot, antes de qualquer outra função.
 *
 * @return ESP_OK em sucesso, ESP_FAIL em erro de configuração UART.
 */
esp_err_t servo_hal_init(void);

/**
 * servo_hal_ping() — Envia instrução PING para um servo e aguarda resposta.
 *
 * Verifica se o servo está presente e responsivo no barramento.
 * Realiza até NB_SERVO_RETRY_MAX tentativas antes de retornar erro.
 *
 * @param id   ID do servo (NB_SERVO_ID_PAN=1 ou NB_SERVO_ID_TILT=2).
 * @return     ESP_OK se servo respondeu, ESP_ERR_TIMEOUT se não respondeu.
 */
esp_err_t servo_hal_ping(uint8_t id);

/**
 * servo_hal_read_raw() — Lê bytes de registradores do servo.
 *
 * Instrução READ do protocolo SCSCL. Lê `len` bytes a partir de `addr`.
 * Realiza até NB_SERVO_RETRY_MAX tentativas em caso de timeout.
 *
 * @param id    ID do servo.
 * @param addr  Endereço inicial do registrador.
 * @param len   Número de bytes a ler (máx 8 por chamada).
 * @param buf   Buffer de saída (deve ter pelo menos `len` bytes).
 * @return      ESP_OK em sucesso, ESP_ERR_TIMEOUT ou ESP_ERR_INVALID_RESPONSE.
 */
esp_err_t servo_hal_read_raw(uint8_t id, uint8_t addr, uint8_t len, uint8_t *buf);

/**
 * servo_hal_read_position() — Lê posição atual do servo (0–1023).
 *
 * @param id   ID do servo.
 * @param pos  Saída: posição em unidades brutas (10-bit, 0–1023).
 * @return     ESP_OK em sucesso.
 */
esp_err_t servo_hal_read_position(uint8_t id, uint16_t *pos);

/**
 * servo_hal_read_load() — Lê carga atual do servo (0–1000, ~0.1%).
 *
 * Bit 10 indica direção: 0=CCW, 1=CW. Bits 9:0 são magnitude.
 *
 * @param id    ID do servo.
 * @param load  Saída: carga bruta (11-bit com bit de direção).
 * @return      ESP_OK em sucesso.
 */
esp_err_t servo_hal_read_load(uint8_t id, uint16_t *load);

/**
 * servo_hal_read_temperature() — Lê temperatura interna do servo em °C.
 *
 * @param id    ID do servo.
 * @param temp  Saída: temperatura em graus Celsius.
 * @return      ESP_OK em sucesso.
 */
esp_err_t servo_hal_read_temperature(uint8_t id, uint8_t *temp);

/**
 * servo_hal_read_voltage() — Lê tensão de alimentação do servo.
 *
 * O valor retornado está em décimos de volt (ex: 60 = 6.0V).
 *
 * @param id      ID do servo.
 * @param voltage Saída: tensão em décimos de volt.
 * @return        ESP_OK em sucesso.
 */
esp_err_t servo_hal_read_voltage(uint8_t id, uint8_t *voltage);

/* Funções de escrita (write_position, enable/disable_torque) estão em
 * servo_hal_write.h — header privado para motion_service e motion_safety. */

/* ── Diagnóstico de bus (F08) ────────────────────────────────────────────── */

typedef struct {
    uint32_t timeouts;  /**< Timeouts aguardando resposta do servo (por tentativa). */
    uint32_t errors;    /**< Erros de header/ID/checksum recebidos.                 */
} servo_hal_bus_stats_t;

/**
 * servo_hal_get_bus_stats() — Retorna contadores acumulados de erros do bus.
 *
 * Thread-safe (leitura atômica). Útil para medir impacto de EMI com WiFi ativo.
 * Os contadores são cumulativos desde o init e nunca são zerados em runtime.
 *
 * @param out  Buffer de saída. Silenciosamente ignorado se NULL.
 */
void servo_hal_get_bus_stats(servo_hal_bus_stats_t *out);

/**
 * servo_hal_deinit() — Libera recursos UART.
 *
 * Chamado apenas em shutdown controlado ou teste. Em operação normal não
 * é necessário.
 */
void servo_hal_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_SERVO_HAL_H */
