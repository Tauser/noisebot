"""Live Opus validation runner for the real firmware path."""

from __future__ import annotations

import json
import time
from dataclasses import dataclass
from typing import Any, Callable

from .voice_ab import VoiceAbError, get_json, post_json, wait_for_new_turn


@dataclass(frozen=True)
class OpusLiveTrial:
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
    enable_ok: bool
    disable_ok: bool
    server_opus_confirmed: bool

    def to_dict(self) -> dict[str, Any]:
        return {
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
            "enable_ok": self.enable_ok,
            "disable_ok": self.disable_ok,
            "server_opus_confirmed": self.server_opus_confirmed,
        }


def run_opus_live_trial(
    *,
    phrase: str,
    server_url: str,
    firmware_url: str,
    timeout_s: float,
    input_fn: Callable[[str], str] = input,
    print_fn: Callable[[str], None] = print,
) -> OpusLiveTrial:
    """Enable Opus, wait for one real voice turn, then disable Opus."""

    server_url = server_url.rstrip("/")
    firmware_url = firmware_url.rstrip("/")
    before_metrics = get_json(f"{server_url}/ai/metrics")
    before_session = _as_dict(before_metrics.get("last_voice_session"))
    previous_turn_id = _optional_int(before_session.get("turn_id"))
    before_worker = get_json(f"{firmware_url}/api/audio/opus/worker")

    enable_payload: dict[str, Any] = {}
    disable_payload: dict[str, Any] = {}
    server_opus_confirmed = False
    try:
        enable_payload = post_json(f"{firmware_url}/api/audio/opus/transport/enable")
        if not enable_payload.get("ok") or not enable_payload.get("opus_enabled"):
            raise VoiceAbError(f"falha ao ligar Opus: {enable_payload}")
        server_opus_confirmed = _wait_for_server_opus(
            server_url=server_url,
            timeout_s=min(5.0, max(1.0, timeout_s / 3.0)),
        )
        if not server_opus_confirmed:
            print_fn("Aviso: /ai/status ainda nao confirmou opus_tx; continuando o teste.")

        print_fn(f"Opus ligado. Fale depois do wake word: {phrase}")
        input_fn("Pressione Enter quando o robo terminar a resposta: ")

        after_metrics = wait_for_new_turn(
            server_url=server_url,
            previous_turn_id=previous_turn_id,
            timeout_s=timeout_s,
        )
        after_worker = get_json(f"{firmware_url}/api/audio/opus/worker")
    finally:
        try:
            disable_payload = post_json(f"{firmware_url}/api/audio/opus/transport/disable")
        except VoiceAbError as exc:
            disable_payload = {"ok": False, "error": str(exc)}

    session = _as_dict(after_metrics.get("last_voice_session"))
    packets_drained = max(
        0,
        _required_int(after_worker.get("opus_packet_drained"))
        - _required_int(before_worker.get("opus_packet_drained")),
    )
    packet_drops = max(
        0,
        _required_int(after_worker.get("opus_packet_drops"))
        - _required_int(before_worker.get("opus_packet_drops")),
    )
    encoded_bytes = max(
        0,
        _required_int(after_worker.get("opus_packet_bytes_total"))
        - _required_int(before_worker.get("opus_packet_bytes_total")),
    )
    quality = str(session.get("transcript_quality") or "")
    transcript = str(session.get("transcript") or "")
    turn_id = _optional_int(session.get("turn_id"))
    total_samples = _optional_int(session.get("total_samples"))
    ok = (
        turn_id is not None
        and turn_id != previous_turn_id
        and quality.lower() in {"good", "ok"}
        and bool(transcript.strip())
        and packets_drained > 0
        and packet_drops == 0
        and bool(disable_payload.get("ok"))
    )

    return OpusLiveTrial(
        phrase=phrase,
        ok=ok,
        turn_id=turn_id,
        outcome=str(session.get("outcome") or ""),
        transcript_quality=quality,
        transcript=transcript,
        discard_reason=str(session.get("discard_reason") or ""),
        total_samples=total_samples,
        stt_ms=_optional_float(session.get("stt_ms")),
        duration_ms=_optional_float(session.get("duration_ms")),
        packets_drained=packets_drained,
        packet_drops=packet_drops,
        encoded_bytes=encoded_bytes,
        enable_ok=bool(enable_payload.get("ok")),
        disable_ok=bool(disable_payload.get("ok")),
        server_opus_confirmed=server_opus_confirmed,
    )


def format_opus_live_markdown(trial: OpusLiveTrial) -> str:
    status = "OK" if trial.ok else "FALHOU"
    return "\n".join(
        [
            "# Opus Live",
            "",
            f"- Status: {status}",
            f"- Turno: {trial.turn_id if trial.turn_id is not None else ''}",
            f"- Qualidade STT: {trial.transcript_quality or trial.outcome}",
            f"- Descarte: {trial.discard_reason}",
            f"- Samples: {trial.total_samples if trial.total_samples is not None else ''}",
            f"- Pacotes drenados: {trial.packets_drained}",
            f"- Drops Opus: {trial.packet_drops}",
            f"- Bytes Opus: {trial.encoded_bytes}",
            f"- Server confirmou Opus: {'sim' if trial.server_opus_confirmed else 'nao'}",
            f"- Transcript: {trial.transcript}",
            "",
        ]
    )


def format_opus_live_json(trial: OpusLiveTrial) -> str:
    return json.dumps(trial.to_dict(), ensure_ascii=False, indent=2)


def _wait_for_server_opus(*, server_url: str, timeout_s: float) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        last_status = get_json(f"{server_url}/ai/status")
        features = last_status.get("features", [])
        if isinstance(features, list) and "opus_tx" in features:
            return True
        time.sleep(0.2)
    return False


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


__all__ = [
    "OpusLiveTrial",
    "format_opus_live_json",
    "format_opus_live_markdown",
    "run_opus_live_trial",
]
