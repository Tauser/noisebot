"""Live barge-in validation runner for the real robot path."""

from __future__ import annotations

import json
import time
from dataclasses import dataclass
from typing import Any, Callable

from .codec_v2_live import CodecV2LiveGuard, CodecV2LiveStats
from .voice_ab import VoiceAbError, get_json


@dataclass(frozen=True)
class BargeLiveTrial:
    phrase: str
    codec: str
    ok: bool
    interrupted_turn_id: int | None
    interruption_cancel_ms: float | None
    transcript: str
    reply: str
    discard_reason: str
    outcome: str
    packets_drained: int = 0
    packet_drops: int = 0
    encoded_bytes: int = 0
    enable_ok: bool = True
    disable_ok: bool = True
    server_codec_confirmed: bool = True

    def to_dict(self) -> dict[str, Any]:
        return {
            "phrase": self.phrase,
            "codec": self.codec,
            "ok": self.ok,
            "interrupted_turn_id": self.interrupted_turn_id,
            "interruption_cancel_ms": self.interruption_cancel_ms,
            "transcript": self.transcript,
            "reply": self.reply,
            "discard_reason": self.discard_reason,
            "outcome": self.outcome,
            "packets_drained": self.packets_drained,
            "packet_drops": self.packet_drops,
            "encoded_bytes": self.encoded_bytes,
            "enable_ok": self.enable_ok,
            "disable_ok": self.disable_ok,
            "server_codec_confirmed": self.server_codec_confirmed,
        }


def run_barge_live_trial(
    *,
    phrase: str,
    server_url: str,
    timeout_s: float,
    codec: str = "pcm16",
    firmware_url: str | None = None,
    input_fn: Callable[[str], str] = input,
    print_fn: Callable[[str], None] = print,
) -> BargeLiveTrial:
    """Wait for a real interrupted voice session after a user-driven test."""

    server_url = server_url.rstrip("/")
    before = get_json(f"{server_url}/ai/metrics")
    before_turn_id = _max_turn_id(before)
    before_interrupted = _turns_interrupted_count(before)
    before_cancel_count = _interruption_cancel_count(before)

    with CodecV2LiveGuard(codec=codec, server_url=server_url, firmware_url=firmware_url) as guard:
        print_fn(f"[{codec}] Inicie uma resposta longa: {phrase}")
        print_fn("Quando o robo estiver falando, interrompa com wake word e uma frase curta.")
        input_fn("Pressione Enter quando a interrupcao terminar: ")

        payload = _wait_for_barge_session(
            server_url=server_url,
            previous_turn_id=before_turn_id,
            previous_interrupted=before_interrupted,
            previous_cancel_count=before_cancel_count,
            timeout_s=timeout_s,
        )
    codec_stats = guard.stats()
    session = payload["session"]
    metrics = payload["metrics"]
    cancel_ms = _interruption_cancel_ms(metrics)
    aggregate_barge = bool(payload.get("aggregate_barge"))
    ok = aggregate_barge or (
        session.get("outcome") == "interrupted"
        and session.get("discard_reason") == "barge_in"
        and (cancel_ms is None or cancel_ms <= 400.0)
    )
    ok = ok and _codec_ok(codec_stats)
    return BargeLiveTrial(
        phrase=phrase,
        codec=codec,
        ok=ok,
        interrupted_turn_id=_optional_int(session.get("turn_id")),
        interruption_cancel_ms=cancel_ms,
        transcript=str(session.get("transcript") or ""),
        reply=str(session.get("reply") or ""),
        discard_reason=str(session.get("discard_reason") or ""),
        outcome=str(session.get("outcome") or ""),
        packets_drained=codec_stats.packets_drained,
        packet_drops=codec_stats.packet_drops,
        encoded_bytes=codec_stats.encoded_bytes,
        enable_ok=codec_stats.enable_ok,
        disable_ok=codec_stats.disable_ok,
        server_codec_confirmed=codec_stats.server_codec_confirmed,
    )


def format_barge_live_markdown(trial: BargeLiveTrial) -> str:
    status = "OK" if trial.ok else "FALHOU"
    return "\n".join(
        [
            "# Barge-in Live",
            "",
            f"- Status: {status}",
            f"- Codec: {trial.codec}",
            f"- Turno interrompido: {trial.interrupted_turn_id if trial.interrupted_turn_id is not None else ''}",
            f"- Cancelamento: {_fmt_float(trial.interruption_cancel_ms)} ms",
            f"- Opus packets/drops/bytes: {trial.packets_drained}/{trial.packet_drops}/{trial.encoded_bytes}",
            f"- Outcome: {trial.outcome}",
            f"- Descarte: {trial.discard_reason}",
            f"- Transcript: {trial.transcript}",
            f"- Reply parcial: {trial.reply}",
            "",
        ]
    )


def format_barge_live_json(trial: BargeLiveTrial) -> str:
    return json.dumps(trial.to_dict(), ensure_ascii=False, indent=2)


def _wait_for_barge_session(
    *,
    server_url: str,
    previous_turn_id: int | None,
    previous_interrupted: int,
    previous_cancel_count: int,
    timeout_s: float,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_s
    last_payload: dict[str, Any] | None = None
    while time.monotonic() < deadline:
        last_payload = get_json(f"{server_url}/ai/metrics")
        for session in _recent_sessions(last_payload):
            turn_id = _optional_int(session.get("turn_id"))
            if previous_turn_id is not None and turn_id is not None and turn_id <= previous_turn_id:
                continue
            if session.get("outcome") == "interrupted" or session.get("discard_reason") == "barge_in":
                return {"metrics": last_payload, "session": session, "aggregate_barge": False}
        latest_session = _latest_new_session(last_payload, previous_turn_id)
        if latest_session is not None:
            interrupted_count = _turns_interrupted_count(last_payload)
            cancel_count = _interruption_cancel_count(last_payload)
            if interrupted_count > previous_interrupted or cancel_count > previous_cancel_count:
                return {
                    "metrics": last_payload,
                    "session": latest_session,
                    "aggregate_barge": True,
                }
        time.sleep(0.5)
    raise VoiceAbError(f"timeout aguardando barge-in em /ai/metrics: {last_payload}")


def _recent_sessions(payload: dict[str, Any]) -> list[dict[str, Any]]:
    sessions = payload.get("recent_voice_sessions")
    if isinstance(sessions, list):
        return [item for item in sessions if isinstance(item, dict)]
    session = payload.get("last_voice_session")
    return [session] if isinstance(session, dict) else []


def _max_turn_id(payload: dict[str, Any]) -> int | None:
    values = [
        turn_id
        for turn_id in (_optional_int(session.get("turn_id")) for session in _recent_sessions(payload))
        if turn_id is not None
    ]
    return max(values) if values else None


def _latest_new_session(
    payload: dict[str, Any],
    previous_turn_id: int | None,
) -> dict[str, Any] | None:
    newest: dict[str, Any] | None = None
    newest_turn_id: int | None = None
    for session in _recent_sessions(payload):
        turn_id = _optional_int(session.get("turn_id"))
        if turn_id is None:
            continue
        if previous_turn_id is not None and turn_id <= previous_turn_id:
            continue
        if newest_turn_id is None or turn_id > newest_turn_id:
            newest = session
            newest_turn_id = turn_id
    return newest


def _interruption_cancel_ms(payload: dict[str, Any]) -> float | None:
    latency = payload.get("latency_ms")
    if not isinstance(latency, dict):
        return None
    item = latency.get("interruption_cancel")
    if not isinstance(item, dict):
        return None
    return _optional_float(item.get("p95") or item.get("p50"))


def _interruption_cancel_count(payload: dict[str, Any]) -> int:
    latency = payload.get("latency_ms")
    if not isinstance(latency, dict):
        return 0
    item = latency.get("interruption_cancel")
    if not isinstance(item, dict):
        return 0
    return _optional_int(item.get("count")) or 0


def _turns_interrupted_count(payload: dict[str, Any]) -> int:
    turns = payload.get("turns")
    if not isinstance(turns, dict):
        return 0
    return _optional_int(turns.get("interrupted")) or 0


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


def _fmt_float(value: float | None) -> str:
    return "" if value is None else f"{value:.1f}"


def _codec_ok(stats: CodecV2LiveStats) -> bool:
    if stats.codec == "pcm16":
        return stats.disable_ok and stats.server_codec_confirmed
    return (
        stats.enable_ok
        and stats.disable_ok
        and stats.server_codec_confirmed
        and stats.packets_drained > 0
        and stats.packet_drops == 0
        and stats.encoded_bytes > 0
    )


__all__ = [
    "BargeLiveTrial",
    "format_barge_live_json",
    "format_barge_live_markdown",
    "run_barge_live_trial",
]
