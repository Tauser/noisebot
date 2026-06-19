/*
 * test_servo_packet.c — Testa a matemática pura do protocolo SCSCL (SCS0009).
 *
 * Sem dependência de ESP-IDF ou FreeRTOS. Replica as funções puras de
 * servo_hal.c (scs_checksum + construção/parse de pacote) e verifica
 * checksum, layout de bytes e validação de resposta.
 *
 * Build:
 *   cmake -S test -B build_test && cmake --build build_test
 * Executar:
 *   ctest --test-dir build_test --output-on-failure
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

/* ── Spec do protocolo SCSCL ──────────────────────────────────────────────
 * Instrução: 0xFF 0xFF [ID] [LEN] [INSTR] [PARAMS...] [CHECKSUM]
 * Resposta:  0xFF 0xFF [ID] [LEN] [ERROR] [PARAMS...] [CHECKSUM]
 * LEN  = INSTR(1) + PARAMS + CHECKSUM(1)
 * CHECKSUM = ~(ID + LEN + INSTR + sum(PARAMS)) & 0xFF
 */

#define SCS_HEADER_BYTE   0xFFu
#define SCS_INSTR_PING    0x01u
#define SCS_INSTR_READ    0x02u
#define SCS_INSTR_WRITE   0x03u
#define SCS_PKT_BUF_SIZE  16u

/* ── Reimplementação das funções puras (espelho de servo_hal.c) ─────────── */

static uint8_t scs_checksum(const uint8_t *buf, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += buf[i];
    return (uint8_t)(~sum & 0xFFu);
}

static int build_packet(uint8_t *out, uint8_t id, uint8_t instr,
                        const uint8_t *params, uint8_t param_len)
{
    uint8_t pkt_len = (uint8_t)(2u + param_len);
    out[0] = SCS_HEADER_BYTE;
    out[1] = SCS_HEADER_BYTE;
    out[2] = id;
    out[3] = pkt_len;
    out[4] = instr;
    if (param_len > 0u && params != NULL) memcpy(&out[5], params, param_len);
    out[5 + param_len] = scs_checksum(&out[2], (size_t)(3u + param_len));
    return (int)(6 + param_len);
}

typedef enum {
    PARSE_OK = 0,
    PARSE_BAD_HEADER,
    PARSE_BAD_ID,
    PARSE_BAD_CHECKSUM,
    PARSE_ERROR_BYTE,
} parse_result_t;

static parse_result_t parse_response(const uint8_t *buf, uint8_t total,
                                     uint8_t expected_id,
                                     uint8_t *payload_out, uint8_t payload_len)
{
    if (buf[0] != SCS_HEADER_BYTE || buf[1] != SCS_HEADER_BYTE)
        return PARSE_BAD_HEADER;
    if (buf[2] != expected_id)
        return PARSE_BAD_ID;
    uint8_t chk_calc = scs_checksum(&buf[2], (size_t)(total - 3u));
    if (chk_calc != buf[total - 1u])
        return PARSE_BAD_CHECKSUM;
    if (buf[4] != 0u)
        return PARSE_ERROR_BYTE;
    if (payload_len > 0u && payload_out != NULL)
        memcpy(payload_out, &buf[5], payload_len);
    return PARSE_OK;
}

/* ── Harness mínimo ──────────────────────────────────────────────────────── */

static int s_pass = 0, s_fail = 0;

#define TEST(name, expr) do { \
    if (expr) { s_pass++; } \
    else { fprintf(stderr, "FAIL [%s]: %s\n", name, #expr); s_fail++; } \
} while (0)

/* ── Testes de checksum ───────────────────────────────────────────────────── */

static void test_checksum_basic(void)
{
    /* PING ID=1: ID=1 LEN=2 INSTR=0x01 → sum=4 → ~4=0xFB */
    uint8_t buf[3] = {0x01, 0x02, 0x01};
    TEST("chk_ping_id1", scs_checksum(buf, 3) == 0xFB);
}

static void test_checksum_zero(void)
{
    uint8_t buf[3] = {0x00, 0x00, 0x00};
    TEST("chk_all_zero", scs_checksum(buf, 3) == 0xFF);
}

static void test_checksum_overflow(void)
{
    /* 3 × 0xFF = 765 = 0x2FD → ~0x2FD = 0xFFFFD02 → & 0xFF = 0x02 */
    uint8_t buf[3] = {0xFF, 0xFF, 0xFF};
    TEST("chk_overflow", scs_checksum(buf, 3) == 0x02);
}

/* ── Testes de construção de pacote ──────────────────────────────────────── */

static void test_build_ping(void)
{
    uint8_t pkt[SCS_PKT_BUF_SIZE];
    int sz = build_packet(pkt, 1, SCS_INSTR_PING, NULL, 0);
    TEST("ping_size",  sz == 6);
    TEST("ping_hdr0",  pkt[0] == 0xFF);
    TEST("ping_hdr1",  pkt[1] == 0xFF);
    TEST("ping_id",    pkt[2] == 0x01);
    TEST("ping_len",   pkt[3] == 0x02);     /* INSTR + CHECKSUM */
    TEST("ping_instr", pkt[4] == SCS_INSTR_PING);
    TEST("ping_chk",   pkt[5] == 0xFB);    /* ~(1+2+1) */
}

static void test_build_read(void)
{
    /* READ reg=0x38 len=2 (posição atual do SCS0009) */
    uint8_t params[2] = {0x38, 0x02};
    uint8_t pkt[SCS_PKT_BUF_SIZE];
    int sz = build_packet(pkt, 1, SCS_INSTR_READ, params, 2);
    /* sum = ID(1)+LEN(4)+INSTR(2)+0x38+0x02 = 1+4+2+56+2 = 65 → ~65=0xBE */
    TEST("read_size",  sz == 8);
    TEST("read_len",   pkt[3] == 0x04);
    TEST("read_instr", pkt[4] == SCS_INSTR_READ);
    TEST("read_p0",    pkt[5] == 0x38);
    TEST("read_p1",    pkt[6] == 0x02);
    TEST("read_chk",   pkt[7] == 0xBE);
}

static void test_build_write_position(void)
{
    /* WRITE pos=512 (0x0200 big-endian) time=500ms (0x01F4) para ID=1 */
    uint8_t params[5] = {
        0x2A,           /* REG_GOAL_POSITION_L (endereço inicial) */
        0x02, 0x00,     /* pos H, L */
        0x01, 0xF4,     /* time H, L */
    };
    uint8_t pkt[SCS_PKT_BUF_SIZE];
    int sz = build_packet(pkt, 1, SCS_INSTR_WRITE, params, 5);
    TEST("write_size",   sz == 11);
    TEST("write_len",    pkt[3] == 0x07);
    TEST("write_instr",  pkt[4] == SCS_INSTR_WRITE);
    TEST("write_reg",    pkt[5] == 0x2A);
    TEST("write_pos_h",  pkt[6] == 0x02);
    TEST("write_pos_l",  pkt[7] == 0x00);
    TEST("write_time_h", pkt[8] == 0x01);
    TEST("write_time_l", pkt[9] == 0xF4);
    /* Checksum verificado por round-trip */
    uint8_t expected_chk = scs_checksum(&pkt[2], (size_t)(sz - 3));
    TEST("write_chk",    pkt[10] == expected_chk);
}

/* ── Testes de parse de resposta ─────────────────────────────────────────── */

static void test_parse_valid_ping_response(void)
{
    /* PING ID=1 OK: 0xFF 0xFF 01 02 00 CHK; chk=~(1+2+0)=0xFC */
    uint8_t resp[6] = {0xFF, 0xFF, 0x01, 0x02, 0x00, 0xFC};
    TEST("parse_ping_ok", parse_response(resp, 6, 1, NULL, 0) == PARSE_OK);
}

static void test_parse_bad_header(void)
{
    uint8_t resp[6] = {0xFE, 0xFF, 0x01, 0x02, 0x00, 0xFC};
    TEST("parse_bad_hdr", parse_response(resp, 6, 1, NULL, 0) == PARSE_BAD_HEADER);
}

static void test_parse_bad_id(void)
{
    /* Resposta de ID=2 com checksum correto: ~(2+2+0)=0xFB */
    uint8_t resp[6] = {0xFF, 0xFF, 0x02, 0x02, 0x00, 0xFB};
    TEST("parse_bad_id", parse_response(resp, 6, 1, NULL, 0) == PARSE_BAD_ID);
}

static void test_parse_bad_checksum(void)
{
    uint8_t resp[6] = {0xFF, 0xFF, 0x01, 0x02, 0x00, 0x00};
    TEST("parse_bad_chk", parse_response(resp, 6, 1, NULL, 0) == PARSE_BAD_CHECKSUM);
}

static void test_parse_servo_error(void)
{
    /* Servo reporta erro 0x04 (overheating): chk=~(1+2+4)=0xF8 */
    uint8_t resp[6] = {0xFF, 0xFF, 0x01, 0x02, 0x04, 0xF8};
    TEST("parse_error_byte", parse_response(resp, 6, 1, NULL, 0) == PARSE_ERROR_BYTE);
}

static void test_parse_read_payload(void)
{
    /* READ 2 bytes ID=1: payload=0x02 0x7A (pos=634, confirmado em maio 2026)
     * chk cobre {01,04,00,02,7A}: sum=1+4+0+2+0x7A=129=0x81 → ~0x81=0x7E */
    uint8_t resp[8] = {0xFF, 0xFF, 0x01, 0x04, 0x00, 0x02, 0x7A, 0x7E};
    uint8_t payload[2];
    parse_result_t r = parse_response(resp, 8, 1, payload, 2);
    TEST("parse_read_ok",    r == PARSE_OK);
    TEST("parse_read_pos_h", payload[0] == 0x02);
    TEST("parse_read_pos_l", payload[1] == 0x7A);
    /* Big-endian: H<<8 | L = 634 */
    uint16_t pos = (uint16_t)((uint16_t)payload[0] << 8u | (uint16_t)payload[1]);
    TEST("parse_read_pos",   pos == 634u);
}

static void test_roundtrip_checksum(void)
{
    /* Pacote construído deve passar na validação de checksum */
    uint8_t params[2] = {0x28, 0x01};  /* torque enable */
    uint8_t pkt[SCS_PKT_BUF_SIZE];
    int sz = build_packet(pkt, 2, SCS_INSTR_WRITE, params, 2);
    uint8_t recomputed = scs_checksum(&pkt[2], (size_t)(sz - 3));
    TEST("roundtrip_chk", recomputed == pkt[sz - 1]);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    test_checksum_basic();
    test_checksum_zero();
    test_checksum_overflow();
    test_build_ping();
    test_build_read();
    test_build_write_position();
    test_parse_valid_ping_response();
    test_parse_bad_header();
    test_parse_bad_id();
    test_parse_bad_checksum();
    test_parse_servo_error();
    test_parse_read_payload();
    test_roundtrip_checksum();

    printf("%d/%d testes passaram\n", s_pass, s_pass + s_fail);
    return s_fail > 0 ? 1 : 0;
}
