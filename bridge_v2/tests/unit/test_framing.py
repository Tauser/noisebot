"""Testes de bridgev2.protocol.framing -- CRC, encode/decode, edge cases."""
from __future__ import annotations

import pytest

from bridgev2.protocol.framing import (
    SOF, FRAME_OVERHEAD,
    crc8, encode_frame, decode_frames,
)
from bridgev2.protocol.messages import MSG_HELLO, MSG_AUDIO_CHUNK, MSG_EVENT


# ── CRC-8/SMBUS ───────────────────────────────────────────────────────────────

class TestCrc8:
    def test_empty(self):
        assert crc8(b"") == 0

    def test_single_byte(self):
        # CRC-8/SMBUS de [0x01]: processa 1 byte
        result = crc8(b"\x01")
        assert isinstance(result, int)
        assert 0 <= result <= 255

    def test_known_vector(self):
        # CRC-8/SMBUS de b"123456789" == 0xF4 (vetor de referencia padrao)
        assert crc8(b"123456789") == 0xF4

    def test_deterministic(self):
        data = b"\xAB\xCD\xEF"
        assert crc8(data) == crc8(data)

    def test_different_inputs_different_crcs(self):
        assert crc8(b"\x01") != crc8(b"\x02")

    def test_byte_range(self):
        for b in range(256):
            r = crc8(bytes([b]))
            assert 0 <= r <= 255

    def test_type_byte_matters(self):
        # Mesmos dados, tipos diferentes -> CRC diferente
        c1 = crc8(bytes([MSG_HELLO]) + b"\x00\x01")
        c2 = crc8(bytes([MSG_AUDIO_CHUNK]) + b"\x00\x01")
        assert c1 != c2


# ── encode_frame ──────────────────────────────────────────────────────────────

class TestEncodeFrame:
    def test_empty_payload(self):
        frame = encode_frame(MSG_HELLO)
        assert len(frame) == FRAME_OVERHEAD
        assert frame[0] == SOF
        # LEN = 0
        assert frame[1] == 0
        assert frame[2] == 0
        # TYPE
        assert frame[3] == MSG_HELLO

    def test_payload_length_in_header(self):
        payload = b"\x01\x02\x03"
        frame = encode_frame(MSG_AUDIO_CHUNK, payload)
        assert frame[1] == 3   # LEN_LO
        assert frame[2] == 0   # LEN_HI
        assert len(frame) == FRAME_OVERHEAD + 3

    def test_large_payload_length(self):
        # LEN = 300 = 0x012C; LO=0x2C HI=0x01
        payload = b"\xFF" * 300
        frame = encode_frame(MSG_AUDIO_CHUNK, payload)
        assert frame[1] == 0x2C
        assert frame[2] == 0x01
        assert len(frame) == FRAME_OVERHEAD + 300

    def test_payload_embedded(self):
        payload = b"\xDE\xAD\xBE\xEF"
        frame = encode_frame(MSG_EVENT, payload)
        extracted = frame[4:4 + len(payload)]
        assert extracted == payload

    def test_crc_is_last_byte(self):
        payload = b"\x01\x02"
        frame = encode_frame(MSG_HELLO, payload)
        expected_crc = crc8(bytes([MSG_HELLO]) + payload)
        assert frame[-1] == expected_crc

    def test_crc_covers_type_and_payload(self):
        # Alterar tipo -> CRC diferente
        f1 = encode_frame(MSG_HELLO, b"\x01")
        f2 = encode_frame(MSG_EVENT, b"\x01")
        assert f1[-1] != f2[-1]

    def test_payload_too_large_raises(self):
        with pytest.raises(ValueError, match="excede"):
            encode_frame(MSG_HELLO, b"\x00" * 65536)

    def test_zero_len_frame_roundtrip(self):
        frame = encode_frame(0x42)
        buf = bytearray(frame)
        frames = decode_frames(buf)
        assert len(frames) == 1
        msg_type, payload = frames[0]
        assert msg_type == 0x42
        assert payload == b""


# ── decode_frames ─────────────────────────────────────────────────────────────

class TestDecodeFrames:
    def test_single_frame(self):
        payload = b"\xDE\xAD"
        frame = encode_frame(MSG_HELLO, payload)
        buf = bytearray(frame)
        frames = decode_frames(buf)
        assert len(frames) == 1
        assert frames[0] == (MSG_HELLO, payload)
        assert len(buf) == 0  # buffer consumido

    def test_multiple_frames_contiguous(self):
        f1 = encode_frame(MSG_HELLO, b"\x01")
        f2 = encode_frame(MSG_EVENT, b"\x02\x03")
        buf = bytearray(f1 + f2)
        frames = decode_frames(buf)
        assert len(frames) == 2
        assert frames[0] == (MSG_HELLO, b"\x01")
        assert frames[1] == (MSG_EVENT, b"\x02\x03")
        assert len(buf) == 0

    def test_incomplete_frame_stays_in_buf(self):
        frame = encode_frame(MSG_HELLO, b"\x01\x02\x03")
        # Entrega so metade
        buf = bytearray(frame[:4])
        frames = decode_frames(buf)
        assert len(frames) == 0
        assert len(buf) == 4  # nao consumiu nada

    def test_garbage_before_sof_skipped(self):
        garbage = b"\x00\xFF\x12"
        frame = encode_frame(MSG_HELLO, b"\xAA")
        buf = bytearray(garbage + frame)
        frames = decode_frames(buf)
        assert len(frames) == 1
        assert frames[0][0] == MSG_HELLO

    def test_bad_crc_frame_discarded(self):
        frame = bytearray(encode_frame(MSG_HELLO, b"\x01"))
        frame[-1] ^= 0xFF  # corrompe CRC
        buf = bytearray(frame)
        frames = decode_frames(buf)
        assert len(frames) == 0
        assert len(buf) == 0  # foi consumido (descartado)

    def test_bad_crc_then_good_frame(self):
        bad = bytearray(encode_frame(MSG_HELLO, b"\x01"))
        bad[-1] ^= 0xFF
        good = encode_frame(MSG_EVENT, b"\x02")
        buf = bytearray(bad) + bytearray(good)
        frames = decode_frames(buf)
        assert len(frames) == 1
        assert frames[0][0] == MSG_EVENT

    def test_buf_consumed_in_place(self):
        """decode_frames() deve modificar buf in-place via del buf[:n]."""
        f = encode_frame(MSG_HELLO, b"hello")
        extra = b"\xAB"  # SOF solitario -- frame incompleto
        buf = bytearray(f) + bytearray(extra)
        frames = decode_frames(buf)
        assert len(frames) == 1
        # O extra solitario fica no buf (frame incompleto)
        assert bytes(buf) == extra

    def test_empty_buffer(self):
        buf = bytearray()
        assert decode_frames(buf) == []

    def test_roundtrip_large_payload(self):
        payload = bytes(range(256)) * 4  # 1024 bytes
        frame = encode_frame(MSG_AUDIO_CHUNK, payload)
        buf = bytearray(frame)
        frames = decode_frames(buf)
        assert len(frames) == 1
        assert frames[0] == (MSG_AUDIO_CHUNK, payload)
