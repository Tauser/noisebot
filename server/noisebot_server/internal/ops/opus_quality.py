"""Offline Opus quality diagnostics for captured voice samples."""
from __future__ import annotations

from dataclasses import asdict, dataclass
from pathlib import Path
import math
import wave

import numpy as np

from ..transport.opus_codec import OPUS_SAMPLE_RATE_HZ, roundtrip_pcm


@dataclass(frozen=True)
class OpusQualityResult:
    name: str
    path: str
    bitrate: int
    duration_s: float
    input_bytes: int
    opus_bytes: int
    packet_count: int
    compression_ratio: float
    rms_original: float
    rms_decoded: float
    rms_error: float
    mae: float
    max_abs_error: int
    snr_db: float | None
    correlation: float | None
    polarity_inverted: bool
    alignment_lag_ms: float
    decoded_padding_ms: float

    def to_dict(self) -> dict:
        return asdict(self)


def analyze_opus_quality(path: str | Path, *, bitrates: list[int]) -> list[OpusQualityResult]:
    """Measure Opus roundtrip loss for one WAV or every WAV in a directory."""
    root = Path(path)
    wav_paths = [root] if root.is_file() else sorted(root.glob("*.wav"))
    if not wav_paths:
        raise ValueError(f"Nenhum WAV encontrado em {root}")
    if not bitrates:
        raise ValueError("Informe pelo menos um bitrate")

    results: list[OpusQualityResult] = []
    for wav_path in wav_paths:
        pcm = _read_mono_16k_pcm(wav_path)
        for bitrate in bitrates:
            results.append(_analyze_one(wav_path, pcm, bitrate=bitrate))
    return results


def summarize_opus_quality(results: list[OpusQualityResult]) -> dict:
    by_bitrate: dict[int, list[OpusQualityResult]] = {}
    for result in results:
        by_bitrate.setdefault(result.bitrate, []).append(result)

    summary: dict[str, dict] = {}
    for bitrate, group in sorted(by_bitrate.items()):
        snr_values = [item.snr_db for item in group if item.snr_db is not None]
        corr_values = [item.correlation for item in group if item.correlation is not None]
        summary[str(bitrate)] = {
            "count": len(group),
            "compression_ratio_avg": _avg(item.compression_ratio for item in group),
            "snr_db_avg": _avg(snr_values) if snr_values else None,
            "snr_db_min": round(min(snr_values), 2) if snr_values else None,
            "correlation_avg": _avg(corr_values) if corr_values else None,
            "rms_error_avg": _avg(item.rms_error for item in group),
            "decoded_padding_ms_max": round(
                max((item.decoded_padding_ms for item in group), default=0.0), 2
            ),
        }
    return {
        "count": len(results),
        "files": len({item.path for item in results}),
        "bitrates": summary,
    }


def format_opus_quality_json(results: list[OpusQualityResult]) -> str:
    import json

    payload = {
        "summary": summarize_opus_quality(results),
        "results": [result.to_dict() for result in results],
    }
    return json.dumps(payload, ensure_ascii=False, indent=2)


def format_opus_quality_markdown(results: list[OpusQualityResult]) -> str:
    summary = summarize_opus_quality(results)
    lines = [
        "# NoiseBot Opus Quality",
        "",
        "## Resumo",
        "",
        f"- Arquivos: {summary['files']}",
        f"- Medicoes: {summary['count']}",
        "",
        "| Bitrate | N | Compressao media | SNR medio | SNR min | Correlacao media | Erro RMS medio | Padding max |",
        "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for bitrate, info in summary["bitrates"].items():
        lines.append(
            "| "
            f"{bitrate} | {info['count']} | {info['compression_ratio_avg']:.4f} | "
            f"{_fmt(info['snr_db_avg'])} | {_fmt(info['snr_db_min'])} | "
            f"{_fmt(info['correlation_avg'], digits=4)} | {info['rms_error_avg']:.2f} | "
            f"{info['decoded_padding_ms_max']:.2f}ms |"
        )

    lines.extend([
        "",
        "## Amostras",
        "",
        "| Arquivo | Bitrate | Duracao | Opus KB | Compressao | SNR | Corr | Lag | Polaridade | RMS in/out | Erro RMS | Padding |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: |",
    ])
    for result in sorted(results, key=lambda item: (item.name, item.bitrate)):
        lines.append(
            "| "
            f"{result.name} | {result.bitrate} | {result.duration_s:.3f}s | "
            f"{result.opus_bytes / 1024.0:.2f} | {result.compression_ratio:.4f} | "
            f"{_fmt(result.snr_db)} | {_fmt(result.correlation, digits=4)} | "
            f"{result.alignment_lag_ms:.2f}ms | {'invertida' if result.polarity_inverted else 'normal'} | "
            f"{result.rms_original:.1f}/{result.rms_decoded:.1f} | "
            f"{result.rms_error:.1f} | {result.decoded_padding_ms:.2f}ms |"
        )
    return "\n".join(lines) + "\n"


def _read_mono_16k_pcm(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as wav:
        channels = wav.getnchannels()
        sample_width = wav.getsampwidth()
        sample_rate = wav.getframerate()
        frames = wav.getnframes()
        raw = wav.readframes(frames)

    if channels != 1:
        raise ValueError(f"{path}: esperado WAV mono; channels={channels}")
    if sample_width != 2:
        raise ValueError(f"{path}: esperado PCM 16-bit; sample_width={sample_width}")
    if sample_rate != OPUS_SAMPLE_RATE_HZ:
        raise ValueError(f"{path}: esperado {OPUS_SAMPLE_RATE_HZ} Hz; sample_rate={sample_rate}")
    return np.frombuffer(raw, dtype=np.int16).copy()


def _analyze_one(path: Path, pcm: np.ndarray, *, bitrate: int) -> OpusQualityResult:
    decoded_bytes, stats = roundtrip_pcm(pcm, bitrate=bitrate)
    decoded = np.frombuffer(decoded_bytes, dtype=np.int16)
    common = min(pcm.size, decoded.size)
    if common == 0:
        raise ValueError(f"{path}: WAV vazio")

    original_raw = pcm[:common].astype(np.float64)
    decoded_raw = decoded[:common].astype(np.float64)
    lag_samples = _estimate_alignment_lag(original_raw, decoded_raw)
    original, decoded_common = _align_by_lag(original_raw, decoded_raw, lag_samples)
    signed_correlation = _signed_correlation(original, decoded_common)
    polarity_inverted = signed_correlation is not None and signed_correlation < 0.0
    if polarity_inverted:
        decoded_common = -decoded_common
    error = original - decoded_common
    rms_original = _rms(original)
    rms_decoded = _rms(decoded_common)
    rms_error = _rms(error)
    snr = None
    if rms_original > 0 and rms_error > 0:
        snr = 20.0 * math.log10(rms_original / rms_error)
    elif rms_original > 0:
        snr = 99.0

    correlation = None
    if rms_original > 0 and rms_decoded > 0:
        corr = float(np.corrcoef(original, decoded_common)[0, 1])
        if not math.isnan(corr):
            correlation = corr

    padding_samples = max(0, decoded.size - pcm.size)
    return OpusQualityResult(
        name=path.name,
        path=str(path),
        bitrate=bitrate,
        duration_s=round(pcm.size / float(OPUS_SAMPLE_RATE_HZ), 3),
        input_bytes=stats.input_bytes,
        opus_bytes=stats.opus_bytes,
        packet_count=stats.packet_count,
        compression_ratio=round(stats.compression_ratio, 4),
        rms_original=round(rms_original, 2),
        rms_decoded=round(rms_decoded, 2),
        rms_error=round(rms_error, 2),
        mae=round(float(np.mean(np.abs(error))), 2),
        max_abs_error=int(np.max(np.abs(error))),
        snr_db=round(snr, 2) if snr is not None else None,
        correlation=round(correlation, 4) if correlation is not None else None,
        polarity_inverted=polarity_inverted,
        alignment_lag_ms=round((lag_samples / float(OPUS_SAMPLE_RATE_HZ)) * 1000.0, 2),
        decoded_padding_ms=round((padding_samples / float(OPUS_SAMPLE_RATE_HZ)) * 1000.0, 2),
    )


def _rms(values: np.ndarray) -> float:
    if values.size == 0:
        return 0.0
    return float(np.sqrt(np.mean(np.square(values))))


def _estimate_alignment_lag(
    original: np.ndarray,
    decoded: np.ndarray,
    *,
    max_lag_ms: int = 120,
    stride: int = 8,
) -> int:
    if original.size < 2 or decoded.size < 2:
        return 0
    max_lag = min(
        int(OPUS_SAMPLE_RATE_HZ * max_lag_ms / 1000),
        max(0, min(original.size, decoded.size) // 4),
    )
    if max_lag == 0:
        return 0

    original_ds = original[::stride]
    decoded_ds = decoded[::stride]
    max_lag_ds = max(1, max_lag // stride)
    best_lag = 0
    best_score = -1.0
    for lag in range(-max_lag_ds, max_lag_ds + 1):
        left, right = _align_by_lag(original_ds, decoded_ds, lag)
        score = _correlation_score(left, right)
        if score > best_score:
            best_score = score
            best_lag = lag
    return best_lag * stride


def _align_by_lag(
    original: np.ndarray,
    decoded: np.ndarray,
    lag_samples: int,
) -> tuple[np.ndarray, np.ndarray]:
    if lag_samples > 0:
        limit = min(original.size, decoded.size - lag_samples)
        return original[:limit], decoded[lag_samples : lag_samples + limit]
    if lag_samples < 0:
        offset = -lag_samples
        limit = min(original.size - offset, decoded.size)
        return original[offset : offset + limit], decoded[:limit]
    limit = min(original.size, decoded.size)
    return original[:limit], decoded[:limit]


def _correlation_score(left: np.ndarray, right: np.ndarray) -> float:
    corr = _signed_correlation(left, right)
    return abs(corr) if corr is not None else 0.0


def _signed_correlation(left: np.ndarray, right: np.ndarray) -> float | None:
    if left.size < 2 or right.size < 2:
        return None
    left_centered = left - float(np.mean(left))
    right_centered = right - float(np.mean(right))
    denom = float(np.linalg.norm(left_centered) * np.linalg.norm(right_centered))
    if denom <= 0.0:
        return None
    return float(np.dot(left_centered, right_centered) / denom)


def _avg(values) -> float:
    collected = list(values)
    if not collected:
        return 0.0
    return round(sum(collected) / len(collected), 4)


def _fmt(value: float | None, *, digits: int = 2) -> str:
    if value is None:
        return "n/a"
    return f"{value:.{digits}f}"
