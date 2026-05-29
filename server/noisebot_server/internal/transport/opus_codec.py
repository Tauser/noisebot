"""Opus codec helpers for server-side experiments.

This module intentionally stays outside the firmware path. It validates the
codec parameters we want before advertising Opus to the robot.
"""

from __future__ import annotations

from dataclasses import dataclass
from fractions import Fraction
from typing import Any

import numpy as np

OPUS_SAMPLE_RATE_HZ = 16000
OPUS_CHANNELS = 1
OPUS_FRAME_MS = 60
OPUS_FRAME_SAMPLES = OPUS_SAMPLE_RATE_HZ * OPUS_FRAME_MS // 1000
OPUS_FRAME_BYTES = OPUS_FRAME_SAMPLES * 2
OPUS_DEFAULT_BITRATE = 24000


class OpusCodecUnavailable(RuntimeError):
    """Raised when PyAV/libopus is not available in the runtime."""


def opus_available() -> bool:
    try:
        import av  # noqa: F401

        return True
    except Exception:
        return False


def _require_av() -> Any:
    try:
        import av

        return av
    except Exception as exc:
        raise OpusCodecUnavailable(
            "PyAV/libopus indisponivel; instale noisebot-server[codec]"
        ) from exc


def _pcm_bytes_to_array(pcm: bytes | bytearray | memoryview | np.ndarray) -> np.ndarray:
    if isinstance(pcm, np.ndarray):
        arr = pcm.astype(np.int16, copy=False).reshape(-1)
    else:
        raw = bytes(pcm)
        if len(raw) % 2:
            raise ValueError("PCM16 precisa ter numero par de bytes")
        arr = np.frombuffer(raw, dtype=np.int16)
    return arr


@dataclass(frozen=True)
class OpusStats:
    input_bytes: int
    packet_count: int
    opus_bytes: int
    decoded_bytes: int

    @property
    def compression_ratio(self) -> float:
        if self.input_bytes == 0:
            return 0.0
        return self.opus_bytes / self.input_bytes


class OpusEncoder:
    def __init__(self, bitrate: int = OPUS_DEFAULT_BITRATE) -> None:
        av = _require_av()
        ctx = av.CodecContext.create("libopus", "w")
        ctx.sample_rate = OPUS_SAMPLE_RATE_HZ
        ctx.layout = "mono"
        ctx.format = "s16"
        ctx.bit_rate = bitrate
        ctx.time_base = Fraction(1, OPUS_SAMPLE_RATE_HZ)
        ctx.options = {"frame_duration": str(OPUS_FRAME_MS)}
        ctx.open()
        self._av = av
        self._ctx = ctx

    def encode_frame(self, pcm: bytes | bytearray | memoryview | np.ndarray) -> bytes:
        arr = _pcm_bytes_to_array(pcm)
        if arr.size != OPUS_FRAME_SAMPLES:
            raise ValueError(
                f"Opus frame precisa de {OPUS_FRAME_SAMPLES} samples; recebeu {arr.size}"
            )
        frame = self._av.AudioFrame.from_ndarray(
            arr.reshape(1, -1),
            format="s16",
            layout="mono",
        )
        frame.sample_rate = OPUS_SAMPLE_RATE_HZ
        frame.time_base = Fraction(1, OPUS_SAMPLE_RATE_HZ)
        packets = self._ctx.encode(frame)
        if len(packets) != 1:
            raise RuntimeError(f"libopus retornou {len(packets)} packets para um frame")
        return bytes(packets[0])


class OpusPacketizer:
    def __init__(self, bitrate: int = OPUS_DEFAULT_BITRATE) -> None:
        self._encoder = OpusEncoder(bitrate=bitrate)
        self._pending = bytearray()

    def feed_pcm(self, pcm: bytes | bytearray | memoryview | np.ndarray) -> list[bytes]:
        arr = _pcm_bytes_to_array(pcm)
        self._pending.extend(arr.astype(np.int16, copy=False).tobytes())
        packets: list[bytes] = []
        while len(self._pending) >= OPUS_FRAME_BYTES:
            frame = bytes(self._pending[:OPUS_FRAME_BYTES])
            del self._pending[:OPUS_FRAME_BYTES]
            packets.append(self._encoder.encode_frame(frame))
        return packets

    def finish(self, pad: bool = True) -> list[bytes]:
        if not self._pending:
            return []
        if not pad:
            raise ValueError("Opus pendente sem padding")
        missing = OPUS_FRAME_BYTES - len(self._pending)
        self._pending.extend(b"\x00" * missing)
        return self.feed_pcm(b"")


class OpusDecoder:
    def __init__(self) -> None:
        av = _require_av()
        from av.audio.resampler import AudioResampler

        ctx = av.CodecContext.create("opus", "r")
        ctx.open()
        self._ctx = ctx
        self._resampler = AudioResampler(format="s16", layout="mono", rate=OPUS_SAMPLE_RATE_HZ)

    def decode_packet(self, packet: bytes | bytearray | memoryview) -> bytes:
        av = _require_av()
        pkt = av.Packet(bytes(packet))
        return self._decode_frames(self._ctx.decode(pkt))

    def finish(self) -> bytes:
        return self._decode_frames(self._ctx.decode(None))

    def _decode_frames(self, frames: list[Any]) -> bytes:
        chunks: list[bytes] = []
        for frame in frames:
            resampled = self._resampler.resample(frame)
            if resampled is None:
                continue
            if not isinstance(resampled, list):
                resampled = [resampled]
            for out in resampled:
                arr = out.to_ndarray().astype(np.int16, copy=False).reshape(-1)
                chunks.append(arr.tobytes())
        return b"".join(chunks)


def roundtrip_pcm(
    pcm: bytes | bytearray | memoryview | np.ndarray,
    bitrate: int = OPUS_DEFAULT_BITRATE,
) -> tuple[bytes, OpusStats]:
    arr = _pcm_bytes_to_array(pcm)
    packetizer = OpusPacketizer(bitrate=bitrate)
    packets = packetizer.feed_pcm(arr)
    packets.extend(packetizer.finish(pad=True))
    decoder = OpusDecoder()
    decoded = b"".join(decoder.decode_packet(packet) for packet in packets)
    decoded += decoder.finish()
    stats = OpusStats(
        input_bytes=arr.nbytes,
        packet_count=len(packets),
        opus_bytes=sum(len(packet) for packet in packets),
        decoded_bytes=len(decoded),
    )
    return decoded, stats
