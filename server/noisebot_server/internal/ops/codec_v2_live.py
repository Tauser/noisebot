"""Shared opt-in Codec v2 live transport helpers."""

from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Any

from .opus_live import _status_confirms_opus
from .voice_ab import VoiceAbError, get_json, post_json


@dataclass(frozen=True)
class CodecV2LiveStats:
    codec: str
    enable_ok: bool
    disable_ok: bool
    server_codec_confirmed: bool
    packets_drained: int
    packet_drops: int
    encoded_bytes: int

    def to_dict(self) -> dict[str, Any]:
        return {
            "codec": self.codec,
            "enable_ok": self.enable_ok,
            "disable_ok": self.disable_ok,
            "server_codec_confirmed": self.server_codec_confirmed,
            "packets_drained": self.packets_drained,
            "packet_drops": self.packet_drops,
            "encoded_bytes": self.encoded_bytes,
        }


class CodecV2LiveGuard:
    """Enable Codec v2 Opus transport for one live test and always rollback."""

    def __init__(self, *, codec: str, server_url: str, firmware_url: str | None) -> None:
        self.codec = codec
        self.server_url = server_url.rstrip("/")
        self.firmware_url = firmware_url.rstrip("/") if firmware_url else ""
        self.enable_ok = codec == "pcm16"
        self.disable_ok = True
        self.server_codec_confirmed = codec == "pcm16"
        self.before: dict[str, Any] = {}
        self.after: dict[str, Any] = {}

    def __enter__(self) -> "CodecV2LiveGuard":
        if self.codec == "pcm16":
            if self.firmware_url:
                _disable_opus(self.firmware_url)
            self.server_codec_confirmed = _wait_for_pcm16(
                server_url=self.server_url,
                timeout_s=3.0,
            )
            return self
        if self.codec != "opus-v2":
            raise VoiceAbError(f"codec live invalido: {self.codec}")
        if not self.firmware_url:
            raise VoiceAbError("--firmware-url ou --host/NOISEBOT_HOST e obrigatorio para opus-v2")

        enable_payload = post_json(f"{self.firmware_url}/api/audio/codec-v2/transport/enable")
        self.enable_ok = bool(enable_payload.get("ok") and enable_payload.get("opus_enabled"))
        if not self.enable_ok:
            raise VoiceAbError(f"falha ao ligar Opus v2: {enable_payload}")
        self.server_codec_confirmed = _wait_for_opus(server_url=self.server_url, timeout_s=5.0)
        self.before = get_json(f"{self.firmware_url}/api/audio/codec-v2")
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        if self.codec != "opus-v2" or not self.firmware_url:
            return
        try:
            self.after = get_json(f"{self.firmware_url}/api/audio/codec-v2")
        except VoiceAbError:
            self.after = {}
        self.disable_ok = _disable_opus(self.firmware_url)

    def stats(self) -> CodecV2LiveStats:
        packets = _opus_packet_delta(self.after, self.before)
        drops = _opus_drop_delta(self.after, self.before)
        encoded_bytes = _opus_bytes_delta(self.after, self.before)
        confirmed = self.server_codec_confirmed or (
            self.codec == "opus-v2" and packets > 0 and drops == 0 and encoded_bytes > 0
        )
        return CodecV2LiveStats(
            codec=self.codec,
            enable_ok=self.enable_ok,
            disable_ok=self.disable_ok,
            server_codec_confirmed=confirmed,
            packets_drained=packets,
            packet_drops=drops,
            encoded_bytes=encoded_bytes,
        )


def _disable_opus(firmware_url: str) -> bool:
    ok = True
    try:
        payload = post_json(f"{firmware_url}/api/audio/codec-v2/transport/disable")
        ok = bool(payload.get("ok")) and ok
    except VoiceAbError:
        ok = False
    try:
        payload = post_json(f"{firmware_url}/api/audio/codec-v2/egress/drain")
        ok = bool(payload.get("ok")) and ok
    except VoiceAbError:
        ok = False
    return ok


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
        if not _status_confirms_opus(get_json(f"{server_url}/ai/status")):
            return True
        time.sleep(0.2)
    return False


def _delta(after: dict[str, Any], before: dict[str, Any], key: str) -> int:
    return max(0, _required_int(after.get(key)) - _required_int(before.get(key)))


def _opus_packet_delta(after: dict[str, Any], before: dict[str, Any]) -> int:
    v2_packets = _delta(after, before, "opus_egress_packets_drained")
    if v2_packets > 0:
        return v2_packets
    return _delta(after, before, "opus_packet_drained")


def _opus_drop_delta(after: dict[str, Any], before: dict[str, Any]) -> int:
    return (
        _delta(after, before, "packet_drops")
        + _delta(after, before, "opus_egress_packet_drops")
        + _delta(after, before, "opus_packet_drops")
    )


def _opus_bytes_delta(after: dict[str, Any], before: dict[str, Any]) -> int:
    v2_bytes = _delta(after, before, "opus_egress_bytes_total")
    if v2_bytes > 0:
        return v2_bytes
    return _delta(after, before, "opus_packet_bytes_total")


def _required_int(value: object) -> int:
    if isinstance(value, bool) or value is None:
        return 0
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


__all__ = [
    "CodecV2LiveGuard",
    "CodecV2LiveStats",
]
