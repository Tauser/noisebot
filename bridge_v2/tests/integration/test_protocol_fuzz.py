"""Fase 10 — Fuzz de protocolo: frames corrompidos não devem travar o bridge.

Verifica que o decoder incremental e o adapter são robustos a:
  - CRC errado
  - Frame truncado
  - Frame de tamanho zero
  - Tipo de mensagem desconhecido
  - Bytes aleatórios no stream
  - SOF ausente (dados fora de sincronismo)
"""
from __future__ import annotations

import struct
from unittest.mock import AsyncMock, MagicMock

import pytest

from bridgev2.protocol.codec import FrameDecoder
from bridgev2.protocol.framing import encode_frame, crc8


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _frame(msg_type: int, data: bytes) -> bytes:
    return encode_frame(msg_type, data)


def _corrupt_crc(frame: bytes) -> bytes:
    """Inverte o último byte (CRC) do frame."""
    return frame[:-1] + bytes([frame[-1] ^ 0xFF])


def _truncate(frame: bytes, keep: int) -> bytes:
    return frame[:keep]


# ---------------------------------------------------------------------------
# FrameDecoder: robustez a bytes malformados
# ---------------------------------------------------------------------------

def _feed(dec: FrameDecoder, data: bytes) -> list:
    """Helper: feed bytes e retorna lista de frames prontos."""
    dec.feed(data)
    return dec.frames()


class TestFrameDecoderFuzz:
    def test_valid_frame_decoded(self):
        dec = FrameDecoder()
        frames = _feed(dec, _frame(0x01, b"\x00\x01" * 8))
        assert len(frames) == 1
        assert frames[0][0] == 0x01

    def test_corrupt_crc_dropped(self):
        dec = FrameDecoder()
        frames = _feed(dec, _corrupt_crc(_frame(0x01, b"\xAB" * 8)))
        assert frames == []

    def test_truncated_frame_no_crash(self):
        dec = FrameDecoder()
        frame = _frame(0x01, b"\x00" * 16)
        truncated = _truncate(frame, len(frame) // 2)
        try:
            _feed(dec, truncated)
        except Exception as exc:
            pytest.fail(f"FrameDecoder levantou exceção em frame truncado: {exc}")

    def test_empty_bytes_no_crash(self):
        dec = FrameDecoder()
        frames = _feed(dec, b"")
        assert frames == []

    def test_random_bytes_no_crash(self):
        dec = FrameDecoder()
        import os
        junk = os.urandom(128)
        try:
            _feed(dec, junk)
        except Exception as exc:
            pytest.fail(f"FrameDecoder levantou exceção em bytes aleatórios: {exc}")

    def test_zero_length_data_frame(self):
        dec = FrameDecoder()
        frames = _feed(dec, _frame(0x02, b""))
        assert len(frames) == 1
        assert frames[0][1] == b""

    def test_unknown_message_type(self):
        dec = FrameDecoder()
        frames = _feed(dec, _frame(0xFE, b"\x00" * 4))
        assert len(frames) == 1
        assert frames[0][0] == 0xFE

    def test_multiple_frames_in_single_feed(self):
        dec = FrameDecoder()
        combined = _frame(0x01, b"\x01") + _frame(0x02, b"\x02\x03")
        frames = _feed(dec, combined)
        assert len(frames) == 2

    def test_frame_split_across_two_feeds(self):
        dec = FrameDecoder()
        frame = _frame(0x01, b"\xAB\xCD" * 4)
        mid = len(frame) // 2
        _feed(dec, frame[:mid])
        frames = _feed(dec, frame[mid:])
        assert len(frames) == 1
        assert frames[0][0] == 0x01

    def test_noise_before_valid_frame_recovered(self):
        dec = FrameDecoder()
        noise = b"\xFF\xFE\xFD\xFC"
        frame = _frame(0x01, b"\x01\x02\x03")
        frames = _feed(dec, noise + frame)
        valid = [f for f in frames if f[0] == 0x01]
        assert len(valid) == 1

    def test_sof_in_middle_of_junk(self):
        dec = FrameDecoder()
        junk_with_sof = b"\x00\xAB\x00\x00\xFF"
        try:
            _feed(dec, junk_with_sof)
        except Exception as exc:
            pytest.fail(f"Exceção em SOF dentro de junk: {exc}")

    def test_repeated_feeds_with_valid_frames(self):
        dec = FrameDecoder()
        frame = _frame(0x01, b"\xAB\xCD" * 128)
        count = 0
        for _ in range(1000):
            count += len(_feed(dec, frame))
        assert count == 1000


# ---------------------------------------------------------------------------
# CRC-8 invariantes
# ---------------------------------------------------------------------------

class TestCrc8:
    def test_same_input_same_crc(self):
        assert crc8(b"hello") == crc8(b"hello")

    def test_different_input_different_crc(self):
        assert crc8(b"hello") != crc8(b"world")

    def test_empty_input(self):
        v = crc8(b"")
        assert isinstance(v, int)
        assert 0 <= v <= 255

    def test_crc_range(self):
        import os
        for _ in range(100):
            data = os.urandom(64)
            c = crc8(data)
            assert 0 <= c <= 255

    def test_single_bit_flip_changes_crc(self):
        data = b"\x01\x02\x03\x04"
        original_crc = crc8(data)
        flipped = bytes([data[0] ^ 0x01]) + data[1:]
        assert crc8(flipped) != original_crc


# ---------------------------------------------------------------------------
# Integridade de encode/decode round-trip
# ---------------------------------------------------------------------------

class TestFrameRoundtrip:
    @pytest.mark.parametrize("msg_type,data", [
        (0x00, b""),
        (0x01, b"\x00\x00" * 256),    # AUDIO_CHUNK (512 bytes)
        (0x10, b"\xFF\xFE" * 128),    # SAY
        (0x11, b"\x01\x00\x00\x00\xD0\x07\x00\x00"),   # EXPR
        (0x15, "Olá, tudo bem?".encode("utf-8")),        # TEXT_SCROLL
    ])
    def test_roundtrip(self, msg_type: int, data: bytes):
        frame = encode_frame(msg_type, data)
        dec = FrameDecoder()
        frames = _feed(dec, frame)
        assert len(frames) == 1
        assert frames[0][0] == msg_type
        assert frames[0][1] == data
