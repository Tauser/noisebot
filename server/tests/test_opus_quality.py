from __future__ import annotations

import math
import wave

import numpy as np
import pytest

from noisebot_server.internal.ops.opus_quality import (
    analyze_opus_quality,
    format_opus_quality_json,
    format_opus_quality_markdown,
    summarize_opus_quality,
)
from noisebot_server.internal.transport import opus_codec


pytestmark = pytest.mark.skipif(
    not opus_codec.opus_available(),
    reason="PyAV/libopus indisponivel",
)


def _write_tone(path, *, seconds: float = 0.24) -> None:
    sample_count = int(opus_codec.OPUS_SAMPLE_RATE_HZ * seconds)
    t = np.arange(sample_count, dtype=np.float32) / float(opus_codec.OPUS_SAMPLE_RATE_HZ)
    samples = (np.sin(2.0 * math.pi * 440.0 * t) * 8000.0).astype(np.int16)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(opus_codec.OPUS_SAMPLE_RATE_HZ)
        wav.writeframes(samples.tobytes())


def test_opus_quality_reports_roundtrip_metrics(tmp_path) -> None:
    wav_path = tmp_path / "voice.wav"
    _write_tone(wav_path)

    results = analyze_opus_quality(wav_path, bitrates=[16000, 24000])
    summary = summarize_opus_quality(results)
    markdown = format_opus_quality_markdown(results)
    json_text = format_opus_quality_json(results)

    assert len(results) == 2
    assert summary["files"] == 1
    assert set(summary["bitrates"]) == {"16000", "24000"}
    assert results[0].packet_count == 4
    assert results[0].opus_bytes < results[0].input_bytes
    assert results[0].correlation is None or results[0].correlation > 0.5
    assert "voice.wav" in markdown
    assert '"summary"' in json_text


def test_opus_quality_rejects_wrong_sample_rate(tmp_path) -> None:
    wav_path = tmp_path / "bad.wav"
    with wave.open(str(wav_path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(8000)
        wav.writeframes(np.zeros(160, dtype=np.int16).tobytes())

    with pytest.raises(ValueError, match="esperado 16000 Hz"):
        analyze_opus_quality(wav_path, bitrates=[24000])
