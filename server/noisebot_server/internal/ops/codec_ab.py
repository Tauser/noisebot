"""Live PCM16 vs Opus validation runner for the real firmware path."""

from __future__ import annotations

import json
import time
from dataclasses import dataclass
from typing import Any, Callable

from .opus_live import _status_confirms_opus
from .voice_ab import VoiceAbError, get_json, post_json, wait_for_new_turn


@dataclass(frozen=True)
class CodecAbTrial:
    codec: str
    phrase: str
    ok: bool
    turn_id: int | None
    outcome: str
    transcript_quality: str
    transcript: str
    discard_reason: str
    total_samples: int | None
    stt_ms: float | None
    duration_ms: float | None
    packets_drained: int
    packet_drops: int
    encoded_bytes: int
    server_codec_confirmed: bool

    def to_dict(self) -> dict[str, Any]:
        return {
            "codec": self.codec,
            "phrase": self.phrase,
            "ok": self.ok,
            "turn_id": self.turn_id,
            "outcome": self.outcome,
            "transcript_quality": self.transcript_quality,
            "transcript": self.transcript,
            "discard_reason": self.discard_reason,
            "total_samples": self.total_samples,
            "stt_ms": self.stt_ms,
            "duration_ms": self.duration_ms,
            "packets_drained": self.packets_drained,
            "packet_drops": self.packet_drops,
            "encoded_bytes": self.encoded_bytes,
            "server_codec_confirmed": self.server_codec_confirmed,
        }


def run_codec_ab_trials(
    *,
    phrases: list[str],
    server_url: str,
    firmware_url: str,
    repeat: int,
    timeout_s: float,
    input_fn: Callable[[str], str] = input,
    print_fn: Callable[[str], None] = print,
) -> list[CodecAbTrial]:
    """Run paired PCM16 and Opus live trials with automatic rollback to PCM16."""

    server_url = server_url.rstrip("/")
    firmware_url = firmware_url.rstrip("/")
    ordered_phrases = [phrase.strip() for phrase in phrases if phrase.strip()]
    if not ordered_phrases:
        raise VoiceAbError("ao menos uma frase e obrigatoria")
    trials: list[CodecAbTrial] = []

    try:
        _disable_opus(firmware_url)
        for _ in range(max(1, repeat)):
            for phrase in ordered_phrases:
                trials.append(
                    _run_one_trial(
                        codec="pcm16",
                        phrase=phrase,
                        server_url=server_url,
                        firmware_url=firmware_url,
                        timeout_s=timeout_s,
                        input_fn=input_fn,
                        print_fn=print_fn,
                    )
                )
                trials.append(
                    _run_one_trial(
                        codec="opus",
                        phrase=phrase,
                        server_url=server_url,
                        firmware_url=firmware_url,
                        timeout_s=timeout_s,
                        input_fn=input_fn,
                        print_fn=print_fn,
                    )
                )
    finally:
        _disable_opus(firmware_url)

    return trials


def format_codec_ab_markdown(trials: list[CodecAbTrial]) -> str:
    lines = [
        "# Codec A/B PCM16 vs Opus",
        "",
        "| codec | ok | turno | qualidade | stt_ms | dur_ms | samples | packets | drops | bytes | transcript |",
        "| --- | --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |",
    ]
    for trial in trials:
        lines.append(
            "| {codec} | {ok} | {turn} | {quality} | {stt} | {duration} | "
            "{samples} | {packets} | {drops} | {bytes} | {transcript} |".format(
                codec=trial.codec,
                ok="sim" if trial.ok else "nao",
                turn=trial.turn_id if trial.turn_id is not None else "",
                quality=trial.transcript_quality or trial.outcome,
                stt=_fmt_float(trial.stt_ms),
                duration=_fmt_float(trial.duration_ms),
                samples=trial.total_samples if trial.total_samples is not None else "",
                packets=trial.packets_drained,
                drops=trial.packet_drops,
                bytes=trial.encoded_bytes,
                transcript=_escape_table(trial.transcript),
            )
        )
    lines.extend(["", *summarize_codec_ab(trials)])
    return "\n".join(lines) + "\n"


def format_codec_ab_json(trials: list[CodecAbTrial]) -> str:
    return json.dumps([trial.to_dict() for trial in trials], ensure_ascii=False, indent=2)


def summarize_codec_ab(trials: list[CodecAbTrial]) -> list[str]:
    pcm = [trial for trial in trials if trial.codec == "pcm16"]
    opus = [trial for trial in trials if trial.codec == "opus"]
    lines = ["## Leitura", ""]
    if not pcm or not opus:
        lines.append("- Colete ao menos um turno PCM16 e um turno Opus.")
        return lines

    pcm_ok = sum(trial.ok for trial in pcm)
    opus_ok = sum(trial.ok for trial in opus)
    opus_drops = sum(trial.packet_drops for trial in opus)
    opus_packets = sum(trial.packets_drained for trial in opus)
    opus_bytes = sum(trial.encoded_bytes for trial in opus)
    pcm_stt = _avg([trial.stt_ms for trial in pcm])
    opus_stt = _avg([trial.stt_ms for trial in opus])

    lines.append(f"- PCM16 ok: {pcm_ok}/{len(pcm)}.")
    lines.append(f"- Opus ok: {opus_ok}/{len(opus)}.")
    lines.append(f"- STT PCM16 medio: {_fmt_float(pcm_stt)} ms.")
    lines.append(f"- STT Opus medio: {_fmt_float(opus_stt)} ms.")
    lines.append(f"- Opus packets drenados: {opus_packets}.")
    lines.append(f"- Opus drops: {opus_drops}.")
    lines.append(f"- Opus bytes: {opus_bytes}.")
    if opus_drops > 0:
        lines.append("- Decisao: Opus permanece opt-in; houve drop.")
    elif opus_ok < pcm_ok:
        lines.append("- Decisao: Opus permanece opt-in; qualidade abaixo de PCM16.")
    elif opus_ok == len(opus) and pcm_ok == len(pcm):
        lines.append("- Decisao: Opus candidato; repetir em sessao longa antes de default.")
    else:
        lines.append("- Decisao: inconclusivo; repetir com mais frases pareadas.")
    return lines


def _run_one_trial(
    *,
    codec: str,
    phrase: str,
    server_url: str,
    firmware_url: str,
    timeout_s: float,
    input_fn: Callable[[str], str],
    print_fn: Callable[[str], None],
) -> CodecAbTrial:
    before_metrics = get_json(f"{server_url}/ai/metrics")
    before_session = _as_dict(before_metrics.get("last_voice_session"))
    previous_turn_id = _optional_int(before_session.get("turn_id"))

    if codec == "opus":
        enable_payload = post_json(f"{firmware_url}/api/audio/opus/transport/enable")
        if not enable_payload.get("ok") or not enable_payload.get("opus_enabled"):
            raise VoiceAbError(f"falha ao ligar Opus: {enable_payload}")
        server_codec_confirmed = _wait_for_opus(server_url=server_url, timeout_s=5.0)
    else:
        _disable_opus(firmware_url)
        server_codec_confirmed = _wait_for_pcm16(server_url=server_url, timeout_s=5.0)

    worker_baseline = get_json(f"{firmware_url}/api/audio/opus/worker")
    print_fn(f"[{codec}] Fale depois do wake word: {phrase}")
    input_fn("Pressione Enter quando o robo terminar a resposta: ")

    after_metrics = wait_for_new_turn(
        server_url=server_url,
        previous_turn_id=previous_turn_id,
        timeout_s=timeout_s,
    )
    after_worker = get_json(f"{firmware_url}/api/audio/opus/worker")
    if codec == "opus":
        _disable_opus(firmware_url)

    return _trial_from_payload(
        codec=codec,
        phrase=phrase,
        previous_turn_id=previous_turn_id,
        metrics=after_metrics,
        worker_before=worker_baseline,
        worker_after=after_worker,
        server_codec_confirmed=server_codec_confirmed,
    )


def _trial_from_payload(
    *,
    codec: str,
    phrase: str,
    previous_turn_id: int | None,
    metrics: dict[str, Any],
    worker_before: dict[str, Any],
    worker_after: dict[str, Any],
    server_codec_confirmed: bool,
) -> CodecAbTrial:
    session = _as_dict(metrics.get("last_voice_session"))
    turn_id = _optional_int(session.get("turn_id"))
    quality = str(session.get("transcript_quality") or "")
    transcript = str(session.get("transcript") or "")
    packets_drained = _delta(worker_after, worker_before, "opus_packet_drained")
    packet_drops = _delta(worker_after, worker_before, "opus_packet_drops")
    encoded_bytes = _delta(worker_after, worker_before, "opus_packet_bytes_total")
    ok = (
        turn_id is not None
        and turn_id != previous_turn_id
        and quality.lower() in {"good", "ok"}
        and bool(transcript.strip())
        and (codec != "opus" or (packets_drained > 0 and packet_drops == 0))
    )
    return CodecAbTrial(
        codec=codec,
        phrase=phrase,
        ok=ok,
        turn_id=turn_id,
        outcome=str(session.get("outcome") or ""),
        transcript_quality=quality,
        transcript=transcript,
        discard_reason=str(session.get("discard_reason") or ""),
        total_samples=_optional_int(session.get("total_samples")),
        stt_ms=_optional_float(session.get("stt_ms")),
        duration_ms=_optional_float(session.get("duration_ms")),
        packets_drained=packets_drained,
        packet_drops=packet_drops,
        encoded_bytes=encoded_bytes,
        server_codec_confirmed=server_codec_confirmed,
    )


def _disable_opus(firmware_url: str) -> None:
    try:
        post_json(f"{firmware_url}/api/audio/opus/transport/disable")
    except VoiceAbError:
        pass


def _wait_for_opus(*, server_url: str, timeout_s: float) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if _status_confirms_opus(get_json(f"{server_url}/ai/status")):
            return True
        time.sleep(0.2)
    return False


def _wait_for_pcm16(*, server_url: str, timeout_s: float) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        status = get_json(f"{server_url}/ai/status")
        if not _status_confirms_opus(status):
            return True
        time.sleep(0.2)
    return False


def _delta(after: dict[str, Any], before: dict[str, Any], key: str) -> int:
    return max(0, _required_int(after.get(key)) - _required_int(before.get(key)))


def _as_dict(value: object) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def _optional_float(value: object) -> float | None:
    if isinstance(value, bool) or value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _optional_int(value: object) -> int | None:
    if isinstance(value, bool) or value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _required_int(value: object) -> int:
    parsed = _optional_int(value)
    return parsed if parsed is not None else 0


def _avg(values: list[float | None]) -> float | None:
    valid = [value for value in values if value is not None]
    if not valid:
        return None
    return sum(valid) / len(valid)


def _fmt_float(value: float | None) -> str:
    if value is None:
        return ""
    return f"{value:.1f}"


def _escape_table(value: str) -> str:
    return value.replace("|", "\\|").replace("\n", " ").strip()


__all__ = [
    "CodecAbTrial",
    "format_codec_ab_json",
    "format_codec_ab_markdown",
    "run_codec_ab_trials",
    "summarize_codec_ab",
]
