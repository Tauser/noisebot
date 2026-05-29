import math

import numpy as np
import pytest

from noisebot_server.internal.transport import opus_codec


pytestmark = pytest.mark.skipif(
    not opus_codec.opus_available(),
    reason="PyAV/libopus indisponivel",
)


def _tone(samples: int, freq_hz: float = 440.0) -> np.ndarray:
    t = np.arange(samples, dtype=np.float32) / float(opus_codec.OPUS_SAMPLE_RATE_HZ)
    return (np.sin(2.0 * math.pi * freq_hz * t) * 8000.0).astype(np.int16)


def test_opus_encoder_emits_single_60ms_packet() -> None:
    encoder = opus_codec.OpusEncoder()
    packet = encoder.encode_frame(_tone(opus_codec.OPUS_FRAME_SAMPLES))

    assert packet
    assert len(packet) < opus_codec.OPUS_FRAME_BYTES


def test_opus_packetizer_aggregates_pcm_chunks_to_60ms_frames() -> None:
    packetizer = opus_codec.OpusPacketizer()
    packets = []

    for chunk in np.array_split(_tone(opus_codec.OPUS_FRAME_SAMPLES * 2), 8):
        packets.extend(packetizer.feed_pcm(chunk))

    assert len(packets) == 2
    assert all(0 < len(packet) < opus_codec.OPUS_FRAME_BYTES for packet in packets)


def test_opus_roundtrip_compresses_and_decodes_pcm() -> None:
    pcm = _tone(opus_codec.OPUS_FRAME_SAMPLES * 4)

    decoded, stats = opus_codec.roundtrip_pcm(pcm)

    assert stats.packet_count == 4
    assert stats.opus_bytes < stats.input_bytes
    assert stats.compression_ratio < 0.25
    assert len(decoded) >= opus_codec.OPUS_FRAME_BYTES * 3
    assert np.frombuffer(decoded, dtype=np.int16).std() > 100
