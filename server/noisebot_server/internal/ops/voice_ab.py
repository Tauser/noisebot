"""Interactive A/B runner for RAW vs firmware AFE voice capture."""

from __future__ import annotations

import json
import time
from dataclasses import dataclass
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


class VoiceAbError(RuntimeError):
    """A/B runner failed to query or control a local endpoint."""


@dataclass(frozen=True)
class VoiceAbTrial:
    mode: str
    phrase: str
    turn_id: int | None
    outcome: str
    transcript_quality: str
    transcript: str
    no_speech_prob: float | None
    avg_logprob: float | None
    compression_ratio: float | None
    stt_ms: float | None
    duration_ms: float | None
    total_samples: int | None
    processed_bridge_chunks: int | None
    processed_bridge_fallbacks: int | None
    processed_output_overruns: int | None

    @classmethod
    def from_payload(
        cls,
        *,
        mode: str,
        phrase: str,
        metrics: dict[str, Any],
        processor: dict[str, Any] | None,
    ) -> "VoiceAbTrial":
        session = _as_dict(metrics.get("last_voice_session"))
        proc = _as_dict(processor)
        return cls(
            mode=mode,
            phrase=phrase,
            turn_id=_optional_int(session.get("turn_id")),
            outcome=str(session.get("outcome") or ""),
            transcript_quality=str(session.get("transcript_quality") or ""),
            transcript=str(session.get("transcript") or ""),
            no_speech_prob=_optional_float(session.get("no_speech_prob")),
            avg_logprob=_optional_float(session.get("avg_logprob")),
            compression_ratio=_optional_float(session.get("compression_ratio")),
            stt_ms=_optional_float(session.get("stt_ms")),
            duration_ms=_optional_float(session.get("duration_ms")),
            total_samples=_optional_int(session.get("total_samples")),
            processed_bridge_chunks=_optional_int(proc.get("processed_bridge_chunks")),
            processed_bridge_fallbacks=_optional_int(proc.get("processed_bridge_fallbacks")),
            processed_output_overruns=_optional_int(proc.get("processed_output_overruns")),
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "mode": self.mode,
            "phrase": self.phrase,
            "turn_id": self.turn_id,
            "outcome": self.outcome,
            "transcript_quality": self.transcript_quality,
            "transcript": self.transcript,
            "no_speech_prob": self.no_speech_prob,
            "avg_logprob": self.avg_logprob,
            "compression_ratio": self.compression_ratio,
            "stt_ms": self.stt_ms,
            "duration_ms": self.duration_ms,
            "total_samples": self.total_samples,
            "processed_bridge_chunks": self.processed_bridge_chunks,
            "processed_bridge_fallbacks": self.processed_bridge_fallbacks,
            "processed_output_overruns": self.processed_output_overruns,
        }


def get_json(url: str, timeout_s: float = 2.0) -> dict[str, Any]:
    request = Request(url, headers={"User-Agent": "NoiseBot-VoiceAB/0.1"})
    try:
        with urlopen(request, timeout=timeout_s) as response:
            data = response.read().decode("utf-8")
    except (HTTPError, URLError, TimeoutError, OSError) as exc:
        raise VoiceAbError(f"{url}: {exc}") from exc
    try:
        payload = json.loads(data)
    except json.JSONDecodeError as exc:
        raise VoiceAbError(f"{url}: resposta nao e JSON") from exc
    if not isinstance(payload, dict):
        raise VoiceAbError(f"{url}: resposta invalida")
    return payload


def post_json(url: str, timeout_s: float = 2.0) -> dict[str, Any]:
    request = Request(
        url,
        data=b"{}",
        method="POST",
        headers={
            "Content-Type": "application/json",
            "User-Agent": "NoiseBot-VoiceAB/0.1",
        },
    )
    try:
        with urlopen(request, timeout=timeout_s) as response:
            data = response.read().decode("utf-8")
    except HTTPError as exc:
        try:
            body = exc.read().decode("utf-8")
        except Exception:
            body = ""
        detail = f"HTTP Error {exc.code}: {exc.reason}"
        if body:
            detail = f"{detail} {body}"
        raise VoiceAbError(f"{url}: {detail}") from exc
    except (URLError, TimeoutError, OSError) as exc:
        raise VoiceAbError(f"{url}: {exc}") from exc
    try:
        payload = json.loads(data)
    except json.JSONDecodeError as exc:
        raise VoiceAbError(f"{url}: resposta nao e JSON") from exc
    if not isinstance(payload, dict):
        raise VoiceAbError(f"{url}: resposta invalida")
    return payload


def post_json_retry_audio_busy(
    url: str,
    *,
    timeout_s: float = 2.0,
    retry_s: float = 10.0,
) -> dict[str, Any]:
    deadline = time.monotonic() + retry_s
    last_error: VoiceAbError | None = None
    while True:
        try:
            return post_json(url, timeout_s=timeout_s)
        except VoiceAbError as exc:
            last_error = exc
            if "audio_busy" not in str(exc) or time.monotonic() >= deadline:
                raise
            time.sleep(0.5)
    raise last_error if last_error else VoiceAbError(f"{url}: retry falhou")


def set_afe_bridge(firmware_url: str, enabled: bool) -> dict[str, Any]:
    firmware_url = firmware_url.rstrip("/")
    path = "start" if enabled else "stop"
    url = f"{firmware_url}/api/audio/processor/bridge/{path}"
    if enabled:
        return post_json_retry_audio_busy(url)
    return post_json(url)


def stop_afe_shadow(firmware_url: str) -> dict[str, Any]:
    return post_json(f"{firmware_url.rstrip('/')}/api/audio/processor/shadow/stop")


def reset_afe_counters(firmware_url: str) -> None:
    """Reset volatile AFE counters by cycling the opt-in bridge mode."""
    firmware_url = firmware_url.rstrip("/")
    try:
        post_json(f"{firmware_url}/api/audio/processor/shadow/stop")
    except VoiceAbError:
        pass
    post_json_retry_audio_busy(f"{firmware_url}/api/audio/processor/bridge/start")
    post_json(f"{firmware_url}/api/audio/processor/bridge/stop")
    try:
        post_json(f"{firmware_url}/api/audio/processor/shadow/stop")
    except VoiceAbError:
        pass


def collect_trial(
    *,
    mode: str,
    phrase: str,
    server_url: str,
    firmware_url: str,
) -> VoiceAbTrial:
    metrics = get_json(f"{server_url.rstrip('/')}/ai/metrics")
    processor = get_json(f"{firmware_url.rstrip('/')}/api/audio/processor")
    return VoiceAbTrial.from_payload(
        mode=mode,
        phrase=phrase,
        metrics=metrics,
        processor=processor,
    )


def wait_for_new_turn(
    *,
    server_url: str,
    previous_turn_id: int | None,
    timeout_s: float,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_s
    last_payload: dict[str, Any] | None = None
    while time.monotonic() < deadline:
        last_payload = get_json(f"{server_url.rstrip('/')}/ai/metrics")
        session = _as_dict(last_payload.get("last_voice_session"))
        turn_id = _optional_int(session.get("turn_id"))
        if turn_id is not None and turn_id != previous_turn_id:
            return last_payload
        time.sleep(0.5)
    if last_payload is not None:
        return last_payload
    raise VoiceAbError("timeout aguardando /ai/metrics")


def format_voice_ab_markdown(trials: list[VoiceAbTrial]) -> str:
    lines = [
        "# Voice A/B RAW vs AFE",
        "",
        "| modo | turno | qualidade | no_speech | logprob | comp | stt_ms | samples | afe_chunks | fallbacks | overruns | transcript |",
        "| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |",
    ]
    for trial in trials:
        lines.append(
            "| {mode} | {turn} | {quality} | {no_speech} | {logprob} | {comp} | "
            "{stt} | {samples} | {chunks} | {fallbacks} | {overruns} | {transcript} |".format(
                mode=trial.mode,
                turn=trial.turn_id if trial.turn_id is not None else "",
                quality=trial.transcript_quality or trial.outcome or "",
                no_speech=_fmt_float(trial.no_speech_prob),
                logprob=_fmt_float(trial.avg_logprob),
                comp=_fmt_float(trial.compression_ratio),
                stt=_fmt_float(trial.stt_ms),
                samples=trial.total_samples if trial.total_samples is not None else "",
                chunks=trial.processed_bridge_chunks if trial.processed_bridge_chunks is not None else "",
                fallbacks=trial.processed_bridge_fallbacks if trial.processed_bridge_fallbacks is not None else "",
                overruns=trial.processed_output_overruns if trial.processed_output_overruns is not None else "",
                transcript=_escape_table(trial.transcript),
            )
        )
    lines.extend(["", *summarize_voice_ab(trials)])
    return "\n".join(lines) + "\n"


def summarize_voice_ab(trials: list[VoiceAbTrial]) -> list[str]:
    raw = [trial for trial in trials if trial.mode == "raw"]
    afe = [trial for trial in trials if trial.mode == "afe"]
    lines = ["## Leitura", ""]
    if not raw or not afe:
        lines.append("- Colete ao menos um turno RAW e um turno AFE para comparar.")
        return lines

    raw_good = sum(_trial_passes_stt(trial) for trial in raw)
    afe_good = sum(_trial_passes_stt(trial) for trial in afe)
    raw_avg_no_speech = _avg([trial.no_speech_prob for trial in raw])
    afe_avg_no_speech = _avg([trial.no_speech_prob for trial in afe])
    afe_overruns = sum(trial.processed_output_overruns or 0 for trial in afe)
    afe_fallbacks = sum(trial.processed_bridge_fallbacks or 0 for trial in afe)

    lines.append(f"- RAW bom: {raw_good}/{len(raw)}.")
    lines.append(f"- AFE bom: {afe_good}/{len(afe)}.")
    lines.append(f"- no_speech RAW médio: {_fmt_float(raw_avg_no_speech)}.")
    lines.append(f"- no_speech AFE médio: {_fmt_float(afe_avg_no_speech)}.")
    lines.append(f"- AFE fallbacks totais: {afe_fallbacks}.")
    lines.append(f"- AFE overruns totais: {afe_overruns}.")
    if afe_overruns > 0:
        lines.append("- Decisão: AFE reprovada por overrun; não promover.")
    elif afe_good < raw_good:
        lines.append("- Decisão: AFE ainda abaixo do RAW; manter opt-in.")
    elif afe_good == len(afe) and (afe_avg_no_speech is None or afe_avg_no_speech <= 0.75):
        lines.append("- Decisão: AFE candidata; coletar mais repetições antes de promover.")
    else:
        lines.append("- Decisão: inconclusivo; repetir com mais frases pareadas.")
    return lines


def _trial_passes_stt(trial: VoiceAbTrial) -> bool:
    quality = trial.transcript_quality.lower()
    return quality in {"good", "ok"} and bool(trial.transcript.strip())


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


def _avg(values: list[float | None]) -> float | None:
    valid = [value for value in values if value is not None]
    if not valid:
        return None
    return sum(valid) / len(valid)


def _fmt_float(value: float | None) -> str:
    if value is None:
        return ""
    return f"{value:.3f}"


def _escape_table(value: str) -> str:
    return value.replace("|", "\\|").replace("\n", " ").strip()


__all__ = [
    "VoiceAbError",
    "VoiceAbTrial",
    "collect_trial",
    "format_voice_ab_markdown",
    "set_afe_bridge",
    "stop_afe_shadow",
    "summarize_voice_ab",
    "wait_for_new_turn",
]
