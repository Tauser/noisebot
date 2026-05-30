"""Live barge-in validation runner for the real robot path."""

from __future__ import annotations

import json
import time
from dataclasses import dataclass
from typing import Any, Callable

from .voice_ab import VoiceAbError, get_json


@dataclass(frozen=True)
class BargeLiveTrial:
    phrase: str
    ok: bool
    interrupted_turn_id: int | None
    interruption_cancel_ms: float | None
    transcript: str
    reply: str
    discard_reason: str
    outcome: str

    def to_dict(self) -> dict[str, Any]:
        return {
            "phrase": self.phrase,
            "ok": self.ok,
            "interrupted_turn_id": self.interrupted_turn_id,
            "interruption_cancel_ms": self.interruption_cancel_ms,
            "transcript": self.transcript,
            "reply": self.reply,
            "discard_reason": self.discard_reason,
            "outcome": self.outcome,
        }


def run_barge_live_trial(
    *,
    phrase: str,
    server_url: str,
    timeout_s: float,
    input_fn: Callable[[str], str] = input,
    print_fn: Callable[[str], None] = print,
) -> BargeLiveTrial:
    """Wait for a real interrupted voice session after a user-driven test."""

    server_url = server_url.rstrip("/")
    before = get_json(f"{server_url}/ai/metrics")
    before_turn_id = _max_turn_id(before)
    before_interrupted = _turns_interrupted_count(before)
    before_cancel_count = _interruption_cancel_count(before)

    print_fn(f"Inicie uma resposta longa: {phrase}")
    print_fn("Quando o robo estiver falando, interrompa com wake word e uma frase curta.")
    input_fn("Pressione Enter quando a interrupcao terminar: ")

    payload = _wait_for_barge_session(
        server_url=server_url,
        previous_turn_id=before_turn_id,
        previous_interrupted=before_interrupted,
        previous_cancel_count=before_cancel_count,
        timeout_s=timeout_s,
    )
    session = payload["session"]
    metrics = payload["metrics"]
    cancel_ms = _interruption_cancel_ms(metrics)
    aggregate_barge = bool(payload.get("aggregate_barge"))
    ok = aggregate_barge or (
        session.get("outcome") == "interrupted"
        and session.get("discard_reason") == "barge_in"
        and (cancel_ms is None or cancel_ms <= 400.0)
    )
    return BargeLiveTrial(
        phrase=phrase,
        ok=ok,
        interrupted_turn_id=_optional_int(session.get("turn_id")),
        interruption_cancel_ms=cancel_ms,
        transcript=str(session.get("transcript") or ""),
        reply=str(session.get("reply") or ""),
        discard_reason=str(session.get("discard_reason") or ""),
        outcome=str(session.get("outcome") or ""),
    )


def format_barge_live_markdown(trial: BargeLiveTrial) -> str:
    status = "OK" if trial.ok else "FALHOU"
    return "\n".join(
        [
            "# Barge-in Live",
            "",
            f"- Status: {status}",
            f"- Turno interrompido: {trial.interrupted_turn_id if trial.interrupted_turn_id is not None else ''}",
            f"- Cancelamento: {_fmt_float(trial.interruption_cancel_ms)} ms",
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


__all__ = [
    "BargeLiveTrial",
    "format_barge_live_json",
    "format_barge_live_markdown",
    "run_barge_live_trial",
]
