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

#define NB_SERVO_REG_PRESENT_POS_L   0x38u   /* posição atual low byte  */
#define NB_SERVO_REG_PRESENT_POS_H   0x39u   /* posição atual high byte */
#define NB_SERVO_REG_PRESENT_SPD_L   0x3Au   /* velocidade atual low    */
#define NB_SERVO_REG_PRESENT_SPD_H   0x3Bu   /* velocidade atual high   */
#define NB_SERVO_REG_PRESENT_LOAD_L  0x3Cu   /* carga atual low         */
#define NB_SERVO_REG_PRESENT_LOAD_H  0x3Du   /* carga atual high        */
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

/**
 * servo_hal_write_position() — Envia posição-alvo e tempo para o servo.
 *
 * Instrução WRITE nos registradores GOAL_POSITION_L (0x2A) e GOAL_TIME_L (0x2C).
 * Fire-and-forget: não aguarda resposta do servo (adequado para paths de safety).
 * Chamada bloqueada se servo_hal não estiver inicializado.
 *
 * @param id       ID do servo.
 * @param pos      Posição alvo em unidades brutas (0–1023).
 * @param time_ms  Tempo para atingir a posição em milissegundos (0 = máx velocidade).
 * @return         ESP_OK em sucesso, ESP_ERR_INVALID_STATE se não inicializado.
 */
esp_err_t servo_hal_write_position(uint8_t id, uint16_t pos, uint16_t time_ms);

/**
 * servo_hal_disable_torque() — Desabilita o torque do motor do servo.
 *
 * Escreve 0 no registrador TORQUE_ENABLE (0x28).
 * Após este comando o servo é livre para ser movido manualmente.
 * Fire-and-forget: não aguarda resposta.
 *
 * @param id  ID do servo.
 * @return    ESP_OK em sucesso.
 */
esp_err_t servo_hal_disable_torque(uint8_t id);

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
