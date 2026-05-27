from __future__ import annotations

import wave

from noisebot_server.internal.ops.audio_analysis import (
    analyze_audio_samples,
    analyze_wav,
    format_audio_samples_markdown,
    summarize_audio_samples,
)


def _write_wav(path, samples: list[int]) -> None:
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(16000)
        wav.writeframes(b"".join(s.to_bytes(2, "little", signed=True) for s in samples))


def test_analyze_wav_reports_level_metrics(tmp_path):
    path = tmp_path / "voice.wav"
    _write_wav(path, [0, 1000, -1000, 32767, -32768])

    result = analyze_wav(path).to_dict()

    assert result["sample_rate_hz"] == 16000
    assert result["channels"] == 1
    assert result["duration_s"] == 0.0
    assert result["peak"] == 32768
    assert result["peak_dbfs"] == 0.0
    assert result["clipping_samples"] == 2
    assert result["clipping_pct"] == 40.0


def test_analyze_wav_rejects_non_pcm16(tmp_path):
    path = tmp_path / "bad.wav"
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(1)
        wav.setframerate(16000)
        wav.writeframes(bytes([1, 2, 3]))

    try:
        analyze_wav(path)
    except ValueError as exc:
        assert "PCM 16-bit" in str(exc)
    else:
        raise AssertionError("expected ValueError")


def test_analyze_audio_samples_extracts_metadata_and_flags(tmp_path):
    _write_wav(tmp_path / "bridge_tx_fala_baixa_123s.wav", [0, 100, -100] * 2000)
    _write_wav(tmp_path / "raw_ruido_ambiente_124s.wav", [0, 32000, -32000] * 2000)

    samples = analyze_audio_samples(tmp_path)
    summary = summarize_audio_samples(samples)
    markdown = format_audio_samples_markdown(samples)

    bridge = next(sample for sample in samples if sample.source == "bridge_tx")
    assert bridge.scenario == "fala_baixa"
    assert bridge.uptime_s == 123
    assert "low_rms" in bridge.flags
    assert summary["sources"] == {"bridge_tx": 1, "raw": 1}
    assert summary["scenarios"] == {"fala_baixa": 1, "ruido_ambiente": 1}
    assert "bridge_tx_fala_baixa_123s.wav" in markdown
