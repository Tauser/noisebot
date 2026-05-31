"""Live validation that robot TTS does not reopen listening by echo."""

from __future__ import annotations

import json
import time
from dataclasses import dataclass
from typing import Any, Callable

from .codec_v2_live import CodecV2LiveGuard, CodecV2LiveStats
from .voice_ab import VoiceAbError, get_json


@dataclass(frozen=True)
class NoEchoLiveTrial:
    phrase: str
    codec: str
    ok: bool
    response_turn_id: int | None
    unexpected_turn_id: int | None
    quiet_window_s: float
    outcome: str
    transcript: str
    discard_reason: str
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
            "response_turn_id": self.response_turn_id,
            "unexpected_turn_id": self.unexpected_turn_id,
            "quiet_window_s": self.quiet_window_s,
            "outcome": self.outcome,
            "transcript": self.transcript,
            "discard_reason": self.discard_reason,
            "packets_drained": self.packets_drained,
            "packet_drops": self.packet_drops,
            "encoded_bytes": self.encoded_bytes,
            "enable_ok": self.enable_ok,
            "disable_ok": self.disable_ok,
            "server_codec_confirmed": self.server_codec_confirmed,
        }


def run_no_echo_live_trial(
    *,
    phrase: str,
    server_url: str,
    quiet_window_s: float,
    timeout_s: float,
    codec: str = "pcm16",
    firmware_url: str | None = None,
    input_fn: Callable[[str], str] = input,
    print_fn: Callable[[str], None] = print,
) -> NoEchoLiveTrial:
    """Ask for one long response, then ensure no extra voice turn appears."""

    server_url = server_url.rstrip("/")
    before = get_json(f"{server_url}/ai/metrics")
    before_turn_id = _max_turn_id(before)

    with CodecV2LiveGuard(codec=codec, server_url=server_url, firmware_url=firmware_url) as guard:
        print_fn(f"[{codec}] Peça ao robo: {phrase}")
        print_fn("Depois que ele terminar de falar, nao fale nada durante a janela de silencio.")
        input_fn("Pressione Enter quando a resposta terminar: ")

        after_response = _wait_for_new_session(
            server_url=server_url,
            previous_turn_id=before_turn_id,
            timeout_s=timeout_s,
        )
        response_turn_id = _optional_int(after_response.get("turn_id"))
        unexpected = _wait_for_unexpected_session(
            server_url=server_url,
            previous_turn_id=response_turn_id,
            quiet_window_s=quiet_window_s,
        )
    codec_stats = guard.stats()
    ok = unexpected is None and _codec_ok(codec_stats)
    session = unexpected or after_response
    return NoEchoLiveTrial(
        phrase=phrase,
        codec=codec,
        ok=ok,
        response_turn_id=response_turn_id,
        unexpected_turn_id=None if unexpected is None else _optional_int(unexpected.get("turn_id")),
        quiet_window_s=quiet_window_s,
        outcome=str(session.get("outcome") or ""),
        transcript=str(session.get("transcript") or ""),
        discard_reason=str(session.get("discard_reason") or ""),
        packets_drained=codec_stats.packets_drained,
        packet_drops=codec_stats.packet_drops,
        encoded_bytes=codec_stats.encoded_bytes,
        enable_ok=codec_stats.enable_ok,
        disable_ok=codec_stats.disable_ok,
        server_codec_confirmed=codec_stats.server_codec_confirmed,
    )


def format_no_echo_live_markdown(trial: NoEchoLiveTrial) -> str:
    status = "OK" if trial.ok else "FALHOU"
    return "\n".join(
        [
            "# No Echo Live",
            "",
            f"- Status: {status}",
            f"- Codec: {trial.codec}",
            f"- Turno da resposta: {trial.response_turn_id if trial.response_turn_id is not None else ''}",
            f"- Turno inesperado: {trial.unexpected_turn_id if trial.unexpected_turn_id is not None else ''}",
            f"- Janela de silencio: {trial.quiet_window_s:.1f}s",
            f"- Opus packets/drops/bytes: {trial.packets_drained}/{trial.packet_drops}/{trial.encoded_bytes}",
            f"- Outcome: {trial.outcome}",
            f"- Descarte: {trial.discard_reason}",
            f"- Transcript: {trial.transcript}",
            "",
        ]
    )


def format_no_echo_live_json(trial: NoEchoLiveTrial) -> str:
    return json.dumps(trial.to_dict(), ensure_ascii=False, indent=2)


def _wait_for_new_session(
    *,
    server_url: str,
    previous_turn_id: int | None,
    timeout_s: float,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_s
    last_payload: dict[str, Any] | None = None
    while time.monotonic() < deadline:
        last_payload = get_json(f"{server_url}/ai/metrics")
        newest = _newest_session_after(last_payload, previous_turn_id)
        if newest is not None:
            return newest
        time.sleep(0.5)
    raise VoiceAbError(f"timeout aguardando turno de resposta em /ai/metrics: {last_payload}")


def _wait_for_unexpected_session(
    *,
    server_url: str,
    previous_turn_id: int | None,
    quiet_window_s: float,
) -> dict[str, Any] | None:
    deadline = time.monotonic() + quiet_window_s
    while time.monotonic() < deadline:
        payload = get_json(f"{server_url}/ai/metrics")
        newest = _newest_session_after(payload, previous_turn_id)
        if newest is not None:
            return newest
        time.sleep(0.5)
    return None


def _newest_session_after(
    payload: dict[str, Any],
    previous_turn_id: int | None,
) -> dict[str, Any] | None:
    candidates: list[dict[str, Any]] = []
    for session in _recent_sessions(payload):
        turn_id = _optional_int(session.get("turn_id"))
        if turn_id is None:
            continue
        if previous_turn_id is not None and turn_id <= previous_turn_id:
            continue
        candidates.append(session)
    if not candidates:
        return None
    return max(candidates, key=lambda item: _optional_int(item.get("turn_id")) or 0)


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


def _optional_int(value: object) -> int | None:
    if isinstance(value, bool) or value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


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
    "NoEchoLiveTrial",
    "format_no_echo_live_json",
    "format_no_echo_live_markdown",
    "run_no_echo_live_trial",
]
