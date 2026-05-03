/*
 * servo_test.c — Diagnóstico e validação de movimento dos servos SCS0009
 *
 * TEMPORÁRIO: remover após validação do hardware.
 *
 * Roda antes do boot_manager_run() → antes do WiFi → sem interferência de RF.
 * Modos compiláveis independentes (ajuste as flags abaixo):
 *
 *   LOOPBACK — valida UART TX/RX isolado (jumper GPIO TX ↔ GPIO RX).
 *   PING     — confirma conectividade com cada servo via TTLinker.
 *   MOTION   — sequência de movimento: centro → min → centro → max → centro.
 *              Acompanhe fisicamente: o servo deve se mover de forma suave.
 */

#include "servo_test.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nb_hw_config.h"

#include <string.h>

static const char *TAG = "servo_test";

#define NB_SERVO_TEST_ENABLE_LOOPBACK 0
#define NB_SERVO_TEST_ENABLE_PING     1
#define NB_SERVO_TEST_ENABLE_MOTION   0

/* Limites de movimento do teste (em steps, 0–1023) */
#define TEST_CENTER  512
#define TEST_MIN     410   /* ≈ −30° do centro */
#define TEST_MAX     614   /* ≈ +30° do centro */
#define TEST_MOVE_MS 700   /* duração de cada passo em ms */
#define TEST_HOLD_MS 400   /* pausa em cada posição antes do próximo passo */

/* ── UART ─────────────────────────────────────────────────────────────────── */

static void uart_init_test(void)
{
    uart_config_t cfg = {
        .baud_rate  = NB_SERVO_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(NB_SERVO_UART_PORT, &cfg);
    uart_set_pin(NB_SERVO_UART_PORT,
                 NB_SERVO_PIN_TX, NB_SERVO_PIN_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(NB_SERVO_UART_PORT, 256, 256, 0, NULL, 0);
    ESP_LOGI(TAG, "UART%d: TX=GPIO%d RX=GPIO%d @ %dbps",
             NB_SERVO_UART_PORT, NB_SERVO_PIN_TX, NB_SERVO_PIN_RX,
             NB_SERVO_BAUD_RATE);
}

/* ── Protocolo SCS raw (sem depender do servo_hal) ───────────────────────── */

static uint8_t scs_chk(const uint8_t *buf, int n)
{
    uint32_t s = 0;
    for (int i = 0; i < n; i++) s += buf[i];
    return (uint8_t)(~s & 0xFFu);
}

/*
 * Envia WRITE_DATA para registrador reg, com data_len bytes de dados.
 * Enviado N vezes para compensar perdas de canal (fire-and-forget).
 */
static void scs_write(uint8_t id, uint8_t reg,
                      const uint8_t *data, uint8_t data_len,
                      int times)
{
    /* FF FF ID LEN INSTR REG DATA... CHK */
    uint8_t pkt[16];
    uint8_t len = (uint8_t)(3u + data_len); /* INSTR + REG + DATA + CHK */
    pkt[0] = 0xFF;
    pkt[1] = 0xFF;
    pkt[2] = id;
    pkt[3] = len;
    pkt[4] = 0x03u; /* WRITE_DATA */
    pkt[5] = reg;
    for (int i = 0; i < data_len; i++) pkt[6 + i] = data[i];
    pkt[6 + data_len] = scs_chk(&pkt[2], (int)(4u + data_len));

    int total = (int)(7u + data_len);

    for (int t = 0; t < times; t++) {
        uart_flush_input(NB_SERVO_UART_PORT);
        uart_write_bytes(NB_SERVO_UART_PORT, (const char *)pkt, (size_t)total);
        uart_wait_tx_done(NB_SERVO_UART_PORT, pdMS_TO_TICKS(10));
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* Habilita torque (TORQUE_ENABLE = 0x28, valor 0x01) */
static void scs_torque(uint8_t id, uint8_t enable, int times)
{
    uint8_t d[] = {enable};
    scs_write(id, 0x28u, d, 1u, times);
}

/* Move servo para pos em time_ms milissegundos (GOAL_POSITION_L = 0x2A) */
static void scs_move(uint8_t id, uint16_t pos, uint16_t time_ms, int times)
{
    uint8_t d[] = {
        (uint8_t)(pos & 0xFFu),     (uint8_t)((pos >> 8u) & 0xFFu),
        (uint8_t)(time_ms & 0xFFu), (uint8_t)((time_ms >> 8u) & 0xFFu),
    };
    scs_write(id, 0x2Au, d, 4u, times);
}

/* ── Teste 1: loopback ────────────────────────────────────────────────────── */

#if NB_SERVO_TEST_ENABLE_LOOPBACK
static void test_loopback(void)
{
    ESP_LOGI(TAG, "--- LOOPBACK (jumper GPIO%d <-> GPIO%d) ---",
             NB_SERVO_PIN_TX, NB_SERVO_PIN_RX);
    const uint8_t send[] = {0xAA, 0x55, 0x01, 0xFF};
    uart_write_bytes(NB_SERVO_UART_PORT, send, sizeof(send));
    uart_wait_tx_done(NB_SERVO_UART_PORT, pdMS_TO_TICKS(10));
    uint8_t recv[sizeof(send)] = {0};
    int len = uart_read_bytes(NB_SERVO_UART_PORT, recv, sizeof(recv),
                              pdMS_TO_TICKS(20));
    if (len == (int)sizeof(send) && memcmp(send, recv, sizeof(send)) == 0) {
        ESP_LOGI(TAG, "LOOPBACK: OK");
    } else {
        ESP_LOGW(TAG, "LOOPBACK: FALHOU (%d/%d bytes)", len, (int)sizeof(send));
    }
}
#endif

/* ── Teste 2: ping ────────────────────────────────────────────────────────── */

#if NB_SERVO_TEST_ENABLE_PING
static bool test_ping_servo(uint8_t id)
{
    uint8_t checksum = (~(id + 0x02u + 0x01u)) & 0xFFu;
    uint8_t ping[]   = {0xFF, 0xFF, id, 0x02, 0x01, checksum};
    uart_flush_input(NB_SERVO_UART_PORT);
    uart_write_bytes(NB_SERVO_UART_PORT, ping, sizeof(ping));
    uart_wait_tx_done(NB_SERVO_UART_PORT, pdMS_TO_TICKS(10));

    uint8_t rx[16] = {0};
    int len = uart_read_bytes(NB_SERVO_UART_PORT, rx, sizeof(rx),
                              pdMS_TO_TICKS(50));
    for (int i = 0; i <= len - 6; i++) {
        uint8_t sum = (uint8_t)(rx[i+2] + rx[i+3] + rx[i+4]);
        if (rx[i]==0xFF && rx[i+1]==0xFF && rx[i+2]==id &&
            rx[i+3]==0x02 && rx[i+4]==0x00 && rx[i+5]==(uint8_t)(~sum&0xFF)) {
            ESP_LOGI(TAG, "PING servo %d: OK", id);
            return true;
        }
    }
    ESP_LOGW(TAG, "PING servo %d: sem resposta (len=%d)", id, len);
    return false;
}
#endif

/* ── Teste 3: sequência de movimento ─────────────────────────────────────── */

#if NB_SERVO_TEST_ENABLE_MOTION
static void test_motion_servo(uint8_t id, const char *name)
{
    ESP_LOGI(TAG, "--- MOTION servo %d (%s) ---", id, name);
    ESP_LOGI(TAG, "Observe o servo fisicamente durante o teste.");

    /* Habilita torque (8 envios para garantir entrega com RF) */
    ESP_LOGI(TAG, "[%s] habilitando torque...", name);
    scs_torque(id, 0x01u, 8);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Centro */
    ESP_LOGI(TAG, "[%s] → CENTRO (%d steps)", name, TEST_CENTER);
    scs_move(id, TEST_CENTER, TEST_MOVE_MS, 5);
    vTaskDelay(pdMS_TO_TICKS(TEST_MOVE_MS + TEST_HOLD_MS));

    /* Mínimo */
    ESP_LOGI(TAG, "[%s] → MIN (%d steps, ≈−30°)", name, TEST_MIN);
    scs_move(id, TEST_MIN, TEST_MOVE_MS, 5);
    vTaskDelay(pdMS_TO_TICKS(TEST_MOVE_MS + TEST_HOLD_MS));

    /* Volta ao centro */
    ESP_LOGI(TAG, "[%s] → CENTRO (%d steps)", name, TEST_CENTER);
    scs_move(id, TEST_CENTER, TEST_MOVE_MS, 5);
    vTaskDelay(pdMS_TO_TICKS(TEST_MOVE_MS + TEST_HOLD_MS));

    /* Máximo */
    ESP_LOGI(TAG, "[%s] → MAX (%d steps, ≈+30°)", name, TEST_MAX);
    scs_move(id, TEST_MAX, TEST_MOVE_MS, 5);
    vTaskDelay(pdMS_TO_TICKS(TEST_MOVE_MS + TEST_HOLD_MS));

    /* Volta ao centro e fica */
    ESP_LOGI(TAG, "[%s] → CENTRO (%d steps) — fim", name, TEST_CENTER);
    scs_move(id, TEST_CENTER, TEST_MOVE_MS, 5);
    vTaskDelay(pdMS_TO_TICKS(TEST_MOVE_MS + TEST_HOLD_MS));

    ESP_LOGI(TAG, "[%s] teste concluido — servo deve estar no centro", name);
}
#endif

/* ── API pública ──────────────────────────────────────────────────────────── */

void nb_servo_test_ping(void)
{
    uart_init_test();

#if NB_SERVO_TEST_ENABLE_LOOPBACK
    test_loopback();
    vTaskDelay(pdMS_TO_TICKS(100));
#endif

#if NB_SERVO_TEST_ENABLE_PING
    ESP_LOGI(TAG, "--- PING SERVO ---");
    bool s1 = test_ping_servo(1);
    vTaskDelay(pdMS_TO_TICKS(100));
    bool s2 = test_ping_servo(2);
    vTaskDelay(pdMS_TO_TICKS(100));
#else
    bool s1 = true, s2 = true; /* assume conectado se ping desabilitado */
#endif

#if NB_SERVO_TEST_ENABLE_MOTION
    /* Só testa movimento se pelo menos um servo respondeu ao ping */
    if (s1 || s2) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (s1) test_motion_servo(1, "PAN");
        vTaskDelay(pdMS_TO_TICKS(300));
        if (s2) test_motion_servo(2, "TILT");
    } else {
        ESP_LOGW(TAG, "MOTION: pulado — nenhum servo respondeu ao ping");
    }
#endif

    uart_driver_delete(NB_SERVO_UART_PORT);
    ESP_LOGI(TAG, "=== diagnostico concluido ===");
}
