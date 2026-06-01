"""Voice Audio v2 release preflight checks."""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any

from .firmware_diag import FirmwareDiagClient
from .voice_ab import get_json


@dataclass(frozen=True)
class ReleaseGate:
    name: str
    ok: bool
    detail: str
    warnings: tuple[str, ...] = ()

    def to_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "ok": self.ok,
            "detail": self.detail,
            "warnings": list(self.warnings),
        }


@dataclass(frozen=True)
class ReleaseCheck:
    ok: bool
    gates: tuple[ReleaseGate, ...]
    codec_v2: dict[str, Any]
    capture_v2: dict[str, Any]
    playback_v2: dict[str, Any]
    metrics: dict[str, Any]

    def to_dict(self) -> dict[str, Any]:
        return {
            "ok": self.ok,
            "gates": [gate.to_dict() for gate in self.gates],
            "codec_v2": self.codec_v2,
            "capture_v2": self.capture_v2,
            "playback_v2": self.playback_v2,
            "metrics": self.metrics,
        }


def run_release_check(
    *,
    firmware_url: str,
    server_url: str,
    timeout_s: float = 1.5,
) -> ReleaseCheck:
    firmware = FirmwareDiagClient(firmware_url.rstrip("/") + "/", timeout_s=timeout_s)
    codec_v2 = firmware.audio_codec_v2_health()
    capture_v2 = firmware.audio_capture_v2_status()
    playback_v2 = firmware.audio_playback_v2_status()
    metrics = get_json(f"{server_url.rstrip('/')}/ai/metrics")

    gates = (
        _codec_gate(codec_v2),
        _capture_gate(capture_v2),
        _playback_gate(playback_v2),
        _metrics_gate(metrics),
    )
    return ReleaseCheck(
        ok=all(gate.ok for gate in gates),
        gates=gates,
        codec_v2=codec_v2,
        capture_v2=capture_v2,
        playback_v2=playback_v2,
        metrics=metrics,
    )


def format_release_check_markdown(check: ReleaseCheck) -> str:
    lines = ["# Voice Audio v2 release preflight", ""]
    lines.append(f"- Status: {'OK' if check.ok else 'FALHOU'}")
    lines.append("")
    for gate in check.gates:
        lines.append(f"## {gate.name}")
        lines.append(f"- OK: {gate.ok}")
        lines.append(f"- Detalhe: {gate.detail}")
        if gate.warnings:
            lines.append(f"- Avisos: {', '.join(gate.warnings)}")
        else:
            lines.append("- Avisos: nenhum")
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def format_release_check_json(check: ReleaseCheck) -> str:
    return json.dumps(check.to_dict(), ensure_ascii=False, indent=2)


def _codec_gate(payload: dict[str, Any]) -> ReleaseGate:
    issues = _list_str(payload.get("issues"))
    warnings = _list_str(payload.get("warnings"))
    ok = bool(payload.get("ok")) and bool(payload.get("healthy")) and payload.get("status") == "ok"
    detail = (
        f"status={payload.get('status')}, format={payload.get('format')}, "
        f"worker={payload.get('worker_state')}, drops={payload.get('packet_drops')}/"
        f"{payload.get('opus_egress_packet_drops')}"
    )
    return ReleaseGate(
        name="Codec v2 / Opus",
        ok=ok,
        detail=detail,
        warnings=tuple(issues + warnings),
    )


def _capture_gate(payload: dict[str, Any]) -> ReleaseGate:
    error = str(payload.get("last_error") or payload.get("error") or "ESP_OK")
    ok = (
        bool(payload.get("ok"))
        and payload.get("real_capture_enabled") is False
        and payload.get("session_active") is False
        and str(payload.get("state") or "") == "IDLE_SESSION"
        and error == "ESP_OK"
    )
    detail = (
        f"enabled={payload.get('real_capture_enabled')}, "
        f"active={payload.get('session_active')}, state={payload.get('state')}, "
        f"error={error}"
    )
    warnings = () if ok else ("capture-v2 deveria estar desligado e idle",)
    return ReleaseGate("Capture v2 default-off", ok, detail, warnings)


def _playback_gate(payload: dict[str, Any]) -> ReleaseGate:
    queue_count = _int(payload.get("say_queue_count"))
    dropped = _int(payload.get("say_chunks_dropped"))
    dropped_listening = _int(payload.get("say_chunks_dropped_listening"))
    error = str(payload.get("last_error") or payload.get("error") or "ESP_OK")
    ok = (
        bool(payload.get("ok"))
        and payload.get("bridge_say_observer") is True
        and payload.get("bridge_say_queue_owner") is True
        and queue_count == 0
        and error == "ESP_OK"
    )
    warnings: list[str] = []
    if dropped:
        warnings.append(f"contador cumulativo say_chunks_dropped={dropped}")
    if dropped_listening:
        warnings.append(f"contador cumulativo say_chunks_dropped_listening={dropped_listening}")
    detail = (
        f"observer={payload.get('bridge_say_observer')}, "
        f"owner={payload.get('bridge_say_queue_owner')}, queue={queue_count}, "
        f"received/played={payload.get('say_chunks_received')}/"
        f"{payload.get('say_chunks_played')}, error={error}"
    )
    return ReleaseGate("Playback v2 SAY", ok, detail, tuple(warnings))


def _metrics_gate(payload: dict[str, Any]) -> ReleaseGate:
    session = payload.get("last_voice_session")
    if not isinstance(session, dict) or not session:
        return ReleaseGate(
            "Métricas de voz",
            True,
            "nenhum turno recente; gate de completude nao aplicavel",
        )

    warnings: list[str] = []
    failures: list[str] = []
    if session.get("tts_completed") is False:
        failures.append("tts_completed=false")
    if session.get("tts_say_end_sent") is False:
        failures.append("tts_say_end_sent=false")
    pages = _int(session.get("text_scroll_pages"))
    sent = _int(session.get("text_scroll_pages_sent"))
    if pages and sent < pages:
        failures.append(f"text_scroll_pages_sent={sent}/{pages}")
    if session.get("text_scroll_truncated") is True:
        warnings.append("TEXT_SCROLL truncado visualmente")

    detail = (
        f"turn={session.get('turn_id')}, outcome={session.get('outcome')}, "
        f"decision={session.get('turn_taking_decision')}, "
        f"tts_completed={session.get('tts_completed')}, pages={sent}/{pages}"
    )
    return ReleaseGate(
        "Métricas de voz",
        ok=not failures,
        detail=detail,
        warnings=tuple(failures + warnings),
    )


def _int(value: Any) -> int:
    try:
        return int(value or 0)
    except (TypeError, ValueError):
        return 0


def _list_str(value: Any) -> list[str]:
    if not isinstance(value, list):
        return []
    return [str(item) for item in value]


__all__ = [
    "ReleaseCheck",
    "ReleaseGate",
    "format_release_check_json",
    "format_release_check_markdown",
    "run_release_check",
]
