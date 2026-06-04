"""Voice Audio v2 release preflight checks."""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any

from .firmware_diag import FirmwareDiagClient
from .voice_ab import get_json

_AUTO_EGRESS_DRAIN_MAX_PACKETS = 1


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
    voice_v2: dict[str, Any]
    codec_v2: dict[str, Any]
    capture_v2: dict[str, Any]
    playback_v2: dict[str, Any]
    metrics: dict[str, Any]

    def to_dict(self) -> dict[str, Any]:
        return {
            "ok": self.ok,
            "gates": [gate.to_dict() for gate in self.gates],
            "voice_v2": self.voice_v2,
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
    voice_v2 = firmware.audio_voice_v2_status()
    codec_v2 = firmware.audio_codec_v2_health()
    capture_v2 = firmware.audio_capture_v2_status()
    playback_v2 = firmware.audio_playback_v2_status()
    voice_v2, codec_v2 = maybe_auto_drain_codec_egress(
        firmware=firmware,
        voice_v2=voice_v2,
        codec_v2=codec_v2,
        capture_v2=capture_v2,
        playback_v2=playback_v2,
    )
    metrics = get_json(f"{server_url.rstrip('/')}/ai/metrics")

    return build_release_check(
        voice_v2=voice_v2,
        codec_v2=codec_v2,
        capture_v2=capture_v2,
        playback_v2=playback_v2,
        metrics=metrics,
    )


def maybe_auto_drain_codec_egress(
    *,
    firmware: FirmwareDiagClient,
    voice_v2: dict[str, Any],
    codec_v2: dict[str, Any],
    capture_v2: dict[str, Any],
    playback_v2: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any]]:
    egress_queue = _int(codec_v2.get("opus_egress_queue_count"))
    if not _should_auto_drain_codec_egress(
        voice_v2=voice_v2,
        codec_v2=codec_v2,
        capture_v2=capture_v2,
        playback_v2=playback_v2,
        egress_queue=egress_queue,
    ):
        return voice_v2, codec_v2

    drain_payload = firmware.audio_codec_v2_egress_drain()
    refreshed_voice_v2 = firmware.audio_voice_v2_status()
    refreshed_codec_v2 = firmware.audio_codec_v2_health()
    refreshed_codec_v2 = dict(refreshed_codec_v2)
    refreshed_codec_v2["auto_egress_drain"] = True
    refreshed_codec_v2["auto_egress_queue_count_before"] = egress_queue
    refreshed_codec_v2["auto_egress_drain_payload"] = drain_payload
    refreshed_codec_v2["auto_egress_drained_packets"] = _int(drain_payload.get("drained_packets"))
    refreshed_codec_v2["auto_egress_queue_count_after"] = _int(
        refreshed_codec_v2.get("opus_egress_queue_count")
    )
    return refreshed_voice_v2, refreshed_codec_v2


def _should_auto_drain_codec_egress(
    *,
    voice_v2: dict[str, Any],
    codec_v2: dict[str, Any],
    capture_v2: dict[str, Any],
    playback_v2: dict[str, Any],
    egress_queue: int,
) -> bool:
    return (
        0 < egress_queue <= _AUTO_EGRESS_DRAIN_MAX_PACKETS
        and bool(codec_v2.get("healthy"))
        and not _list_str(codec_v2.get("issues"))
        and _int(codec_v2.get("packet_drops")) == 0
        and _int(codec_v2.get("opus_egress_packet_drops")) == 0
        and _int(codec_v2.get("opus_codec_error")) == 0
        and bool(voice_v2.get("ok"))
        and voice_v2.get("ready") is True
        and str(voice_v2.get("block_reason") or "") == "none"
        and voice_v2.get("runtime_idle") is True
        and capture_v2.get("session_active") is False
        and playback_v2.get("bridge_say_active") is not True
        and _int(playback_v2.get("say_queue_count")) == 0
    )


def build_release_check(
    *,
    voice_v2: dict[str, Any],
    codec_v2: dict[str, Any],
    capture_v2: dict[str, Any],
    playback_v2: dict[str, Any],
    metrics: dict[str, Any],
) -> ReleaseCheck:
    gates = (
        _voice_gate(voice_v2),
        _codec_gate(codec_v2),
        _capture_gate(capture_v2),
        _playback_gate(playback_v2),
        _metrics_gate(metrics),
    )
    return ReleaseCheck(
        ok=all(gate.ok for gate in gates),
        gates=gates,
        voice_v2=voice_v2,
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


def _voice_gate(payload: dict[str, Any]) -> ReleaseGate:
    ok = (
        bool(payload.get("ok"))
        and payload.get("ready") is True
        and str(payload.get("block_reason") or "") == "none"
    )
    detail = (
        f"ready={payload.get('ready')}, block={payload.get('block_reason')}, "
        f"capture={payload.get('capture_enabled')}/tx={payload.get('capture_tx_enabled')}, "
        f"activity={payload.get('activity_decider_enabled')}, "
        f"codec_worker={payload.get('codec_worker_state')}, "
        f"say_queue={payload.get('playback_say_queue_count')}, "
        f"drops={payload.get('playback_say_drops')}/"
        f"{payload.get('codec_packet_drops')}/"
        f"{payload.get('codec_egress_drops')}"
    )
    warnings: list[str] = []
    if not ok:
        warnings.append(f"voice-v2 block_reason={payload.get('block_reason')}")
    if payload.get("runtime_idle") is False:
        warnings.append("runtime_idle=false")
    heap_internal_bytes = _int(payload.get("audio_io_heap_internal_free_bytes"))
    heap_dma_bytes = _int(payload.get("audio_io_heap_dma_free_bytes"))
    heap_internal_largest = _int(payload.get("audio_io_heap_internal_largest_free_block"))
    heap_dma_largest = _int(payload.get("audio_io_heap_dma_largest_free_block"))
    if 0 < heap_internal_bytes < 16 * 1024:
        warnings.append(
            "audio_io_heap_internal_free_bytes baixo: "
            f"{heap_internal_bytes} (largest={heap_internal_largest})"
        )
    elif heap_internal_bytes == 0:
        heap_internal_kb = _int(payload.get("audio_io_heap_internal_free_kb"))
        if 0 < heap_internal_kb < 16:
            warnings.append(f"audio_io_heap_internal_free_kb baixo: {heap_internal_kb}")
    if 0 < heap_dma_bytes < 16 * 1024:
        warnings.append(
            "audio_io_heap_dma_free_bytes baixo: "
            f"{heap_dma_bytes} (largest={heap_dma_largest})"
        )
    elif heap_dma_bytes == 0:
        heap_dma_kb = _int(payload.get("audio_io_heap_dma_free_kb"))
        if 0 < heap_dma_kb < 16:
            warnings.append(f"audio_io_heap_dma_free_kb baixo: {heap_dma_kb}")
    return ReleaseGate("Voice v2 consolidado", ok, detail, tuple(warnings))


def _codec_gate(payload: dict[str, Any]) -> ReleaseGate:
    issues = _list_str(payload.get("issues"))
    warnings = _list_str(payload.get("warnings"))
    ok = bool(payload.get("ok")) and bool(payload.get("healthy")) and payload.get("status") == "ok"
    detail = (
        f"status={payload.get('status')}, format={payload.get('format')}, "
        f"worker={payload.get('worker_state')}, drops={payload.get('packet_drops')}/"
        f"{payload.get('opus_egress_packet_drops')}"
    )
    if payload.get("auto_egress_drain") is True:
        warnings.append(
            "auto_egress_drain="
            f"{payload.get('auto_egress_drained_packets')}"
            f" ({payload.get('auto_egress_queue_count_before')}->"
            f"{payload.get('auto_egress_queue_count_after')})"
        )
    return ReleaseGate(
        name="Codec v2 / Opus",
        ok=ok,
        detail=detail,
        warnings=tuple(issues + warnings),
    )


def _capture_gate(payload: dict[str, Any]) -> ReleaseGate:
    error = str(payload.get("last_error") or payload.get("error") or "ESP_OK")
    state = str(payload.get("state") or "")
    disabled = payload.get("real_capture_enabled") is False
    controlled = (
        payload.get("real_capture_enabled") is True
        and payload.get("bridge_tx_handoff_enabled") is True
    )
    inactive = payload.get("session_active") is False
    idle_or_retained_done = state in {"IDLE_SESSION", "DONE"}
    dropped_frames = _int(payload.get("dropped_frames"))
    shadow_drops = _int(payload.get("shadow_audio_dropped_chunks"))
    ok = (
        bool(payload.get("ok"))
        and (disabled or controlled)
        and inactive
        and idle_or_retained_done
        and error == "ESP_OK"
        and dropped_frames == 0
        and shadow_drops == 0
    )
    detail = (
        f"enabled={payload.get('real_capture_enabled')}, "
        f"tx_handoff={payload.get('bridge_tx_handoff_enabled')}, "
        f"active={payload.get('session_active')}, state={payload.get('state')}, "
        f"drops={dropped_frames}/{shadow_drops}, error={error}"
    )
    if ok and disabled and state == "DONE":
        warnings = ("capture-v2 reteve a ultima sessao DONE, mas esta desligado e inativo",)
    elif ok and controlled and state == "DONE" and payload.get("bridge_tx_owner") is True:
        warnings = ("capture-v2 controlado reteve o ownership da ultima sessao DONE",)
    elif ok:
        warnings = ()
    elif disabled:
        warnings = ("capture-v2 deveria estar desligado, inativo e sem drops",)
    else:
        warnings = ("capture-v2 controlado deve estar inativo, com handoff ligado e sem drops",)
    return ReleaseGate("Capture v2 controlado", ok, detail, warnings)


def _playback_gate(payload: dict[str, Any]) -> ReleaseGate:
    queue_count = _int(payload.get("say_queue_count"))
    say_begin_count = _int(payload.get("say_begin_count"))
    say_end_count = _int(payload.get("say_end_count"))
    dropped = _int(payload.get("say_chunks_dropped"))
    dropped_listening = _int(payload.get("say_chunks_dropped_listening"))
    error = str(payload.get("last_error") or payload.get("error") or "ESP_OK")
    lifecycle_active = payload.get("bridge_say_active") is True
    lifecycle_balanced = say_begin_count == say_end_count
    ok = (
        bool(payload.get("ok"))
        and payload.get("bridge_say_observer") is True
        and payload.get("bridge_say_queue_owner") is True
        and not lifecycle_active
        and lifecycle_balanced
        and queue_count == 0
        and error == "ESP_OK"
    )
    warnings: list[str] = []
    if lifecycle_active:
        warnings.append("bridge_say_active=true")
    if not lifecycle_balanced:
        warnings.append(f"lifecycle SAY aberto begin/end={say_begin_count}/{say_end_count}")
    if dropped:
        warnings.append(f"contador cumulativo say_chunks_dropped={dropped}")
    if dropped_listening:
        warnings.append(f"contador cumulativo say_chunks_dropped_listening={dropped_listening}")
    detail = (
        f"observer={payload.get('bridge_say_observer')}, "
        f"owner={payload.get('bridge_say_queue_owner')}, queue={queue_count}, "
        f"active={payload.get('bridge_say_active')}, begin/end={say_begin_count}/{say_end_count}, "
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
    "build_release_check",
    "format_release_check_json",
    "format_release_check_markdown",
    "maybe_auto_drain_codec_egress",
    "run_release_check",
]
