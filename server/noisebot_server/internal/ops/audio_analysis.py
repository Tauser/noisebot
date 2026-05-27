"""Analise simples de WAVs gravados pelo firmware."""
from __future__ import annotations

from dataclasses import asdict, dataclass
from pathlib import Path
from collections import Counter, defaultdict
import math
import re
import wave


_FILENAME_RE = re.compile(r"^(?P<source>raw|bridge_tx)_(?P<scenario>.+)_(?P<uptime>\d+)s\.wav$")


@dataclass(frozen=True)
class WavAnalysis:
    path: str
    sample_rate_hz: int
    channels: int
    sample_width_bytes: int
    frames: int
    duration_s: float
    rms: float
    peak: int
    peak_dbfs: float | None
    clipping_samples: int
    clipping_pct: float

    def to_dict(self) -> dict:
        return asdict(self)


@dataclass(frozen=True)
class AudioSampleAnalysis:
    name: str
    source: str | None
    scenario: str | None
    uptime_s: int | None
    analysis: WavAnalysis
    flags: tuple[str, ...]

    def to_dict(self) -> dict:
        payload = {
            "name": self.name,
            "source": self.source,
            "scenario": self.scenario,
            "uptime_s": self.uptime_s,
            "flags": list(self.flags),
        }
        payload.update(self.analysis.to_dict())
        return payload


def analyze_wav(path: str | Path, *, clipping_threshold: int = 32760) -> WavAnalysis:
    """Retorna metricas de nivel para WAV PCM 16-bit.

    O firmware grava mono 16 kHz, mas a funcao valida apenas o essencial para
    permitir analisar amostras copiadas ou convertidas sem acoplar ao SD.
    """
    wav_path = Path(path)
    with wave.open(str(wav_path), "rb") as wav:
        channels = wav.getnchannels()
        sample_width = wav.getsampwidth()
        sample_rate = wav.getframerate()
        frames = wav.getnframes()
        raw = wav.readframes(frames)

    if sample_width != 2:
        raise ValueError(f"WAV precisa ser PCM 16-bit; sample_width={sample_width}")
    if channels <= 0:
        raise ValueError("WAV sem canais")

    sample_count = len(raw) // 2
    if sample_count == 0:
        peak_dbfs = None
        rms = 0.0
        peak = 0
        clipping_samples = 0
    else:
        total_sq = 0
        peak = 0
        clipping_samples = 0
        for i in range(0, len(raw), 2):
            value = int.from_bytes(raw[i : i + 2], "little", signed=True)
            abs_value = abs(value)
            total_sq += value * value
            if abs_value > peak:
                peak = abs_value
            if abs_value >= clipping_threshold:
                clipping_samples += 1
        rms = math.sqrt(total_sq / sample_count)
        peak_dbfs = 20.0 * math.log10(peak / 32768.0) if peak > 0 else None

    duration_s = frames / sample_rate if sample_rate > 0 else 0.0
    clipping_pct = (clipping_samples / sample_count) * 100.0 if sample_count else 0.0

    return WavAnalysis(
        path=str(wav_path),
        sample_rate_hz=sample_rate,
        channels=channels,
        sample_width_bytes=sample_width,
        frames=frames,
        duration_s=round(duration_s, 3),
        rms=round(rms, 2),
        peak=peak,
        peak_dbfs=round(peak_dbfs, 2) if peak_dbfs is not None else None,
        clipping_samples=clipping_samples,
        clipping_pct=round(clipping_pct, 4),
    )


def analyze_audio_samples(path: str | Path) -> list[AudioSampleAnalysis]:
    """Analisa todos os WAVs de uma pasta de amostras da Fase 4."""
    root = Path(path)
    if root.is_file():
        wav_paths = [root]
    else:
        wav_paths = sorted(root.glob("*.wav"))

    samples: list[AudioSampleAnalysis] = []
    for wav_path in wav_paths:
        analysis = analyze_wav(wav_path)
        source, scenario, uptime_s = _parse_sample_name(wav_path.name)
        samples.append(
            AudioSampleAnalysis(
                name=wav_path.name,
                source=source,
                scenario=scenario,
                uptime_s=uptime_s,
                analysis=analysis,
                flags=_quality_flags(analysis, source=source),
            )
        )
    return sorted(
        samples,
        key=lambda item: (
            item.scenario or "",
            item.source or "",
            item.uptime_s if item.uptime_s is not None else -1,
            item.name,
        ),
    )


def summarize_audio_samples(samples: list[AudioSampleAnalysis]) -> dict:
    """Gera um resumo pequeno para dashboard/relatorio."""
    source_counts = Counter(sample.source or "unknown" for sample in samples)
    scenario_counts = Counter(sample.scenario or "unknown" for sample in samples)
    flag_counts: Counter[str] = Counter()
    by_source: dict[str, list[AudioSampleAnalysis]] = defaultdict(list)

    for sample in samples:
        by_source[sample.source or "unknown"].append(sample)
        flag_counts.update(sample.flags)

    source_summary: dict[str, dict] = {}
    for source, group in sorted(by_source.items()):
        rms_values = [item.analysis.rms for item in group]
        peak_dbfs_values = [
            item.analysis.peak_dbfs
            for item in group
            if item.analysis.peak_dbfs is not None
        ]
        source_summary[source] = {
            "count": len(group),
            "rms_avg": round(sum(rms_values) / len(rms_values), 2) if rms_values else 0.0,
            "rms_min": min(rms_values) if rms_values else 0.0,
            "rms_max": max(rms_values) if rms_values else 0.0,
            "peak_dbfs_max": max(peak_dbfs_values) if peak_dbfs_values else None,
            "clipping_pct_max": max((item.analysis.clipping_pct for item in group), default=0.0),
        }

    return {
        "count": len(samples),
        "sources": dict(sorted(source_counts.items())),
        "scenarios": dict(sorted(scenario_counts.items())),
        "flags": dict(sorted(flag_counts.items())),
        "by_source": source_summary,
    }


def format_audio_samples_markdown(samples: list[AudioSampleAnalysis]) -> str:
    """Formata o baseline de amostras como Markdown simples."""
    summary = summarize_audio_samples(samples)
    lines = [
        "# NoiseBot Voice Samples - Fase 4",
        "",
        "## Resumo",
        "",
        f"- Amostras: {summary['count']}",
        f"- Fontes: {_format_counts(summary['sources'])}",
        f"- Cenarios: {_format_counts(summary['scenarios'])}",
        f"- Flags: {_format_counts(summary['flags']) if summary['flags'] else 'nenhuma'}",
        "",
        "## Por Fonte",
        "",
        "| Fonte | N | RMS medio | RMS min | RMS max | Pico max dBFS | Clipping max |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]

    for source, info in summary["by_source"].items():
        peak = info["peak_dbfs_max"]
        peak_text = "n/a" if peak is None else f"{peak:.2f}"
        lines.append(
            "| "
            f"{source} | {info['count']} | {info['rms_avg']:.2f} | "
            f"{info['rms_min']:.2f} | {info['rms_max']:.2f} | "
            f"{peak_text} | "
            f"{info['clipping_pct_max']:.4f}% |"
        )

    lines.extend([
        "",
        "## Amostras",
        "",
        "| Arquivo | Fonte | Cenario | Duracao | RMS | Pico dBFS | Clipping | Flags |",
        "| --- | --- | --- | ---: | ---: | ---: | ---: | --- |",
    ])

    for sample in samples:
        analysis = sample.analysis
        peak = "n/a" if analysis.peak_dbfs is None else f"{analysis.peak_dbfs:.2f}"
        flags = ", ".join(sample.flags) if sample.flags else "-"
        lines.append(
            "| "
            f"{sample.name} | {sample.source or '-'} | {sample.scenario or '-'} | "
            f"{analysis.duration_s:.3f}s | {analysis.rms:.2f} | {peak} | "
            f"{analysis.clipping_pct:.4f}% | {flags} |"
        )

    return "\n".join(lines) + "\n"


def _parse_sample_name(name: str) -> tuple[str | None, str | None, int | None]:
    match = _FILENAME_RE.match(name)
    if match is None:
        return None, None, None
    return (
        match.group("source"),
        match.group("scenario"),
        int(match.group("uptime")),
    )


def _quality_flags(analysis: WavAnalysis, *, source: str | None) -> tuple[str, ...]:
    flags: list[str] = []
    if analysis.channels != 1:
        flags.append("channels")
    if analysis.sample_rate_hz != 16000:
        flags.append("sample_rate")
    if analysis.duration_s < 0.5:
        flags.append("too_short")
    if analysis.clipping_pct > 0:
        flags.append("clipping")
    if analysis.peak_dbfs is not None and analysis.peak_dbfs > -1.0:
        flags.append("hot_peak")
    if source == "bridge_tx" and analysis.rms < 500:
        flags.append("low_rms")
    return tuple(flags)


def _format_counts(counts: dict[str, int]) -> str:
    if not counts:
        return "nenhum"
    return ", ".join(f"{key}={value}" for key, value in counts.items())
