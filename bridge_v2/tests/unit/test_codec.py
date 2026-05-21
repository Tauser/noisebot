"""Testes de bridgev2.protocol.codec -- FrameDecoder incremental."""
from __future__ import annotations

import pytest

from bridgev2.protocol.codec import FrameDecoder
from bridgev2.protocol.framing import encode_frame, FRAME_OVERHEAD
from bridgev2.protocol.messages import MSG_HELLO, MSG_AUDIO_CHUNK, MSG_EVENT, MSG_STATUS


def _make_frame(msg_type: int, payload: bytes = b"") -> bytes:
    return encode_frame(msg_type, payload)


class TestFrameDecoderBasic:
    def test_single_frame_fed_at_once(self):
        dec = FrameDecoder()
        frame = _make_frame(MSG_HELLO, b"\x01\x02")
        dec.feed(frame)
        frames = dec.frames()
        assert len(frames) == 1
        assert frames[0] == (MSG_HELLO, b"\x01\x02")

    def test_frames_cleared_after_read(self):
        dec = FrameDecoder()
        dec.feed(_make_frame(MSG_HELLO))
        _ = dec.frames()
        assert dec.frames() == []

    def test_empty_payload_frame(self):
        dec = FrameDecoder()
        dec.feed(_make_frame(MSG_EVENT))
        frames = dec.frames()
        assert frames == [(MSG_EVENT, b"")]

    def test_multiple_frames_fed_at_once(self):
        dec = FrameDecoder()
        data = _make_frame(MSG_HELLO, b"\x01") + _make_frame(MSG_EVENT, b"\x02\x03")
        dec.feed(data)
        frames = dec.frames()
        assert len(frames) == 2
        assert frames[0] == (MSG_HELLO, b"\x01")
        assert frames[1] == (MSG_EVENT, b"\x02\x03")

    def test_iter_interface(self):
        dec = FrameDecoder()
        dec.feed(_make_frame(MSG_HELLO, b"X"))
        found = list(dec)
        assert found == [(MSG_HELLO, b"X")]


class TestFrameDecoderSplit:
    def test_byte_by_byte(self):
        """Alimentado 1 byte por vez deve produzir o frame completo ao final."""
        dec = FrameDecoder()
        frame = _make_frame(MSG_AUDIO_CHUNK, b"\xAA\xBB\xCC")
        for byte in frame:
            dec.feed(bytes([byte]))
        frames = dec.frames()
        assert len(frames) == 1
        assert frames[0] == (MSG_AUDIO_CHUNK, b"\xAA\xBB\xCC")

    def test_split_at_header_boundary(self):
        dec = FrameDecoder()
        frame = _make_frame(MSG_HELLO, b"\x01\x02\x03\x04")
        # Divide no meio do header (2 bytes + resto)
        dec.feed(frame[:2])
        assert dec.frames() == []
        dec.feed(frame[2:])
        frames = dec.frames()
        assert frames == [(MSG_HELLO, b"\x01\x02\x03\x04")]

    def test_split_between_frames(self):
        dec = FrameDecoder()
        f1 = _make_frame(MSG_HELLO, b"\xDE\xAD")
        f2 = _make_frame(MSG_EVENT, b"\xBE\xEF")
        combined = f1 + f2
        # Divide bem no meio (pode cortar f1 no fim ou f2 no inicio)
        mid = len(f1) + 2
        dec.feed(combined[:mid])
        partial = dec.frames()
        dec.feed(combined[mid:])
        rest = dec.frames()
        all_frames = partial + rest
        assert len(all_frames) == 2
        assert all_frames[0] == (MSG_HELLO, b"\xDE\xAD")
        assert all_frames[1] == (MSG_EVENT, b"\xBE\xEF")

    def test_many_splits_large_payload(self):
        dec = FrameDecoder()
        payload = bytes(range(200))
        frame = _make_frame(MSG_AUDIO_CHUNK, payload)
        # Alimenta em pedacos de 7 bytes
        chunk_size = 7
        for i in range(0, len(frame), chunk_size):
            dec.feed(frame[i:i + chunk_size])
        frames = dec.frames()
        assert len(frames) == 1
        assert frames[0] == (MSG_AUDIO_CHUNK, payload)


class TestFrameDecoderBadData:
    def test_bad_crc_discarded(self):
        dec = FrameDecoder()
        bad = bytearray(_make_frame(MSG_HELLO, b"\x01"))
        bad[-1] ^= 0xFF  # corrompe CRC
        dec.feed(bytes(bad))
        assert dec.frames() == []

    def test_garbage_before_sof(self):
        dec = FrameDecoder()
        garbage = b"\x00\x01\x02\xFE"
        frame = _make_frame(MSG_HELLO, b"\xAA")
        dec.feed(garbage + frame)
        frames = dec.frames()
        assert frames == [(MSG_HELLO, b"\xAA")]

    def test_bad_then_good_frame(self):
        dec = FrameDecoder()
        bad = bytearray(_make_frame(MSG_HELLO, b"\x01"))
        bad[-1] ^= 0x55
        good = _make_frame(MSG_EVENT, b"\x02")
        dec.feed(bytes(bad) + good)
        frames = dec.frames()
        assert len(frames) == 1
        assert frames[0] == (MSG_EVENT, b"\x02")

    def test_only_sof_byte(self):
        """SOF solitario: nao deve produzir frame nem lancar excecao."""
        dec = FrameDecoder()
        dec.feed(b"\xAB")
        assert dec.frames() == []
        assert dec.buffered_bytes == 1


class TestFrameDecoderBufferedBytes:
    def test_buffered_bytes_empty(self):
        dec = FrameDecoder()
        assert dec.buffered_bytes == 0

    def test_buffered_bytes_incomplete_frame(self):
        dec = FrameDecoder()
        frame = _make_frame(MSG_HELLO, b"\x01\x02\x03")
        dec.feed(frame[:3])  # menos de FRAME_OVERHEAD+payload
        assert dec.buffered_bytes == 3

    def test_buffered_bytes_zero_after_complete(self):
        dec = FrameDecoder()
        frame = _make_frame(MSG_HELLO, b"\x01")
        dec.feed(frame)
        _ = dec.frames()
        # Nao ha bytes remanescentes
        assert dec.buffered_bytes == 0


class TestFrameDecoderStress:
    def test_1000_frames_no_loss(self):
        dec = FrameDecoder()
        count = 1000
        # Gera e concatena 1000 frames de tamanhos variados
        raw = b"".join(
            _make_frame(MSG_AUDIO_CHUNK, bytes([i % 256]) * (i % 32))
            for i in range(count)
        )
        # Alimenta em chunks de 64 bytes
        for i in range(0, len(raw), 64):
            dec.feed(raw[i:i + 64])
        frames = dec.frames()
        assert len(frames) == count
