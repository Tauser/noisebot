"""HTTP client for firmware diagnostics endpoints."""

from __future__ import annotations

import json
import os
import time
from dataclasses import dataclass
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urljoin
from urllib.request import Request, urlopen


class FirmwareDiagError(RuntimeError):
    """Firmware diagnostics query failed."""


@dataclass(frozen=True)
class FirmwareDiagClient:
    """Small HTTP client for diagnostic endpoints on the robot firmware."""

    base_url: str
    timeout_s: float = 1.5

    @classmethod
    def from_config(cls, config) -> "FirmwareDiagClient | None":
        explicit = os.environ.get("NOISEBOT_ROBOT_HTTP_URL", "").strip()
        if explicit:
            return cls(explicit.rstrip("/") + "/", _env_float("NOISEBOT_DIAG_TIMEOUT_S", 1.5))

        host = getattr(getattr(config, "transport", None), "host", None)
        if not host:
            return None
        return cls(f"http://{host}/", _env_float("NOISEBOT_DIAG_TIMEOUT_S", 1.5))

    def collect(self) -> dict[str, Any]:
        started = time.perf_counter()
        endpoints = {
            "diag": "api/diag",
            "health": "api/health",
            "version": "api/version",
            "wifi": "api/wifi",
            "audio": "api/audio",
            "audio_processor": "api/audio/processor",
            "camera": "api/camera/status",
            "vision": "api/vision/status",
            "touch": "api/touch",
            "agenda": "api/agenda",
            "config": "api/config/all",
            "ltm": "api/ltm",
        }
        payload: dict[str, Any] = {
            "available": True,
            "base_url": self.base_url.rstrip("/"),
            "latency_ms": None,
            "errors": {},
        }
        errors: dict[str, str] = {}
        for key, path in endpoints.items():
            try:
                payload[key] = self._get_json(path)
            except FirmwareDiagError as exc:
                payload[key] = None
                errors[key] = str(exc)
        payload["latency_ms"] = round((time.perf_counter() - started) * 1000.0)
        payload["errors"] = errors
        payload["available"] = len(errors) < len(endpoints)
        return payload

    def _get_json(self, path: str) -> dict[str, Any] | list[Any]:
        data = self._get_bytes(path).decode("utf-8")
        try:
            payload = json.loads(data)
        except json.JSONDecodeError as exc:
            raise FirmwareDiagError(f"{path}: resposta nao e JSON") from exc
        if not isinstance(payload, (dict, list)):
            raise FirmwareDiagError(f"{path}: resposta invalida")
        return payload

    def _get_bytes(self, path: str) -> bytes:
        url = urljoin(self.base_url, path.lstrip("/"))
        request = Request(url, headers={"User-Agent": "NoiseBot-Server/0.1"})
        try:
            with urlopen(request, timeout=self.timeout_s) as response:
                return response.read()
        except (HTTPError, URLError, TimeoutError, OSError) as exc:
            raise FirmwareDiagError(f"{path}: {exc}") from exc

    def _post_json(self, path: str, payload: dict[str, Any] | None = None) -> dict[str, Any]:
        url = urljoin(self.base_url, path.lstrip("/"))
        body = json.dumps(payload or {}).encode("utf-8")
        request = Request(
            url,
            data=body,
            method="POST",
            headers={
                "Content-Type": "application/json",
                "User-Agent": "NoiseBot-Server/0.1",
            },
        )
        try:
            with urlopen(request, timeout=self.timeout_s) as response:
                data = response.read().decode("utf-8")
        except (HTTPError, URLError, TimeoutError, OSError) as exc:
            raise FirmwareDiagError(f"{path}: {exc}") from exc
        try:
            decoded = json.loads(data)
        except json.JSONDecodeError as exc:
            raise FirmwareDiagError(f"{path}: resposta nao e JSON") from exc
        if not isinstance(decoded, dict):
            raise FirmwareDiagError(f"{path}: resposta invalida")
        return decoded

    def list_audio_files(self) -> dict[str, Any]:
        payload = self._get_json("api/audio/files")
        if not isinstance(payload, dict):
            raise FirmwareDiagError("api/audio/files: resposta invalida")
        return payload

    def download_audio_file(self, name: str) -> bytes:
        if not _valid_audio_filename(name):
            raise FirmwareDiagError("nome de arquivo invalido")
        return self._get_bytes(f"api/audio/file?{urlencode({'name': name})}")

    def audio_processor_status(self) -> dict[str, Any]:
        payload = self._get_json("api/audio/processor")
        if not isinstance(payload, dict):
            raise FirmwareDiagError("api/audio/processor: resposta invalida")
        return payload

    def audio_processor_probe(self) -> dict[str, Any]:
        return self._post_json("api/audio/processor/probe")

    def audio_processor_shadow_start(self) -> dict[str, Any]:
        return self._post_json("api/audio/processor/shadow/start")

    def audio_processor_shadow_stop(self) -> dict[str, Any]:
        return self._post_json("api/audio/processor/shadow/stop")

    def audio_processor_bridge_start(self) -> dict[str, Any]:
        return self._post_json("api/audio/processor/bridge/start")

    def audio_processor_bridge_stop(self) -> dict[str, Any]:
        return self._post_json("api/audio/processor/bridge/stop")

    def audio_opus_worker_status(self) -> dict[str, Any]:
        payload = self._get_json("api/audio/opus/worker")
        if not isinstance(payload, dict):
            raise FirmwareDiagError("api/audio/opus/worker: resposta invalida")
        return payload

    def audio_opus_worker_probe(self) -> dict[str, Any]:
        return self._post_json("api/audio/opus/worker/probe")

    def audio_opus_worker_start(self) -> dict[str, Any]:
        return self._post_json("api/audio/opus/worker/start")

    def audio_opus_worker_stop(self) -> dict[str, Any]:
        return self._post_json("api/audio/opus/worker/stop")

    def audio_opus_worker_encode_test(self) -> dict[str, Any]:
        return self._post_json("api/audio/opus/worker/encode-test")

    def audio_opus_worker_drain_packets(self) -> dict[str, Any]:
        return self._post_json("api/audio/opus/worker/drain-packets")

    def audio_opus_transport_enable(self) -> dict[str, Any]:
        return self._post_json("api/audio/opus/transport/enable")

    def audio_opus_transport_disable(self) -> dict[str, Any]:
        return self._post_json("api/audio/opus/transport/disable")

    def audio_capture_v2_status(self) -> dict[str, Any]:
        payload = self._get_json("api/audio/capture-v2")
        if not isinstance(payload, dict):
            raise FirmwareDiagError("api/audio/capture-v2: resposta invalida")
        return payload

    def audio_capture_v2_replay(self, payload: dict[str, Any] | None = None) -> dict[str, Any]:
        return self._post_json("api/audio/capture-v2/replay", payload)

    def audio_capture_v2_cancel(self) -> dict[str, Any]:
        return self._post_json("api/audio/capture-v2/cancel")

    def audio_playback_v2_status(self) -> dict[str, Any]:
        payload = self._get_json("api/audio/playback-v2")
        if not isinstance(payload, dict):
            raise FirmwareDiagError("api/audio/playback-v2: resposta invalida")
        return payload

    def audio_codec_v2_status(self) -> dict[str, Any]:
        payload = self._get_json("api/audio/codec-v2")
        if not isinstance(payload, dict):
            raise FirmwareDiagError("api/audio/codec-v2: resposta invalida")
        return payload

    def audio_codec_v2_health(self) -> dict[str, Any]:
        return codec_v2_health_from_status(self.audio_codec_v2_status())

    def audio_codec_v2_encode_test(self) -> dict[str, Any]:
        return self._post_json("api/audio/codec-v2/encode-test")

    def audio_codec_v2_drain(self) -> dict[str, Any]:
        return self._post_json("api/audio/codec-v2/drain")

    def audio_codec_v2_egress_drain(self) -> dict[str, Any]:
        return self._post_json("api/audio/codec-v2/egress/drain")

    def audio_codec_v2_reset(self) -> dict[str, Any]:
        return self._post_json("api/audio/codec-v2/reset")

    def audio_codec_v2_opus_encode_test(self) -> dict[str, Any]:
        return self._post_json("api/audio/codec-v2/opus-encode-test")

    def audio_codec_v2_worker_start(self) -> dict[str, Any]:
        return self._post_json("api/audio/codec-v2/worker/start")

    def audio_codec_v2_worker_stop(self) -> dict[str, Any]:
        return self._post_json("api/audio/codec-v2/worker/stop")

    def audio_codec_v2_worker_stress_test(self, packets: int = 10) -> dict[str, Any]:
        return self._post_json("api/audio/codec-v2/worker/stress-test", {"packets": packets})

    def audio_codec_v2_worker_feed_test(self, frames: int = 10) -> dict[str, Any]:
        return self._post_json("api/audio/codec-v2/worker/feed-test", {"frames": frames})

    def audio_codec_v2_bridge_handoff_test(self, frames: int = 10) -> dict[str, Any]:
        return self._post_json("api/audio/codec-v2/bridge-handoff-test", {"frames": frames})

    def audio_codec_v2_transport_enable(self) -> dict[str, Any]:
        return self._post_json("api/audio/codec-v2/transport/enable")

    def audio_codec_v2_transport_disable(self) -> dict[str, Any]:
        return self._post_json("api/audio/codec-v2/transport/disable")

    def audio_codec_v2_overflow_test(self, packets: int = 45) -> dict[str, Any]:
        return self._post_json("api/audio/codec-v2/overflow-test", {"packets": packets})

    def set_voice_audio_v2_capture_enabled(self, enabled: bool) -> dict[str, Any]:
        return self._post_json(
            "api/config",
            {
                "key": "voice_audio_v2_capture_enabled",
                "value": 1 if enabled else 0,
            },
        )

    def set_voice_audio_v2_capture_tx_enabled(self, enabled: bool) -> dict[str, Any]:
        return self._post_json(
            "api/config",
            {
                "key": "voice_audio_v2_capture_tx_enabled",
                "value": 1 if enabled else 0,
            },
        )


def _env_float(key: str, default: float) -> float:
    try:
        return float(os.environ.get(key, default))
    except (TypeError, ValueError):
        return default


def _valid_audio_filename(name: str) -> bool:
    if not name.endswith(".wav") or len(name) < 5 or len(name) > 80:
        return False
    allowed = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.")
    return all(ch in allowed for ch in name)


def codec_v2_health_from_status(status: dict[str, Any]) -> dict[str, Any]:
    issues: list[str] = []
    warnings: list[str] = []
    codec_error = _int_field(status, "opus_codec_error")
    packet_drops = _int_field(status, "packet_drops")
    egress_drops = _int_field(status, "opus_egress_packet_drops")
    egress_queue = _int_field(status, "opus_egress_queue_count")
    queue_count = _int_field(status, "queue_count")
    fmt = str(status.get("format") or "")
    worker_state = str(status.get("worker_state") or "")
    worker_active = bool(status.get("worker_active"))

    if not status.get("ok", False):
        issues.append("codec-v2 status retornou ok=false")
    if str(status.get("error") or "ESP_OK") != "ESP_OK":
        issues.append(f"firmware reportou {status.get('error')}")
    if codec_error:
        issues.append(f"opus_codec_error={codec_error}")
    if packet_drops:
        issues.append(f"packet_drops={packet_drops}")
    if egress_drops:
        issues.append(f"opus_egress_packet_drops={egress_drops}")
    if fmt == "opus" and not worker_active:
        issues.append("transporte Opus ativo sem worker ativo")
    if worker_active and worker_state not in {"running", "stopping"}:
        issues.append(f"worker ativo em estado inesperado: {worker_state or 'vazio'}")
    if egress_queue:
        warnings.append(f"opus_egress_queue_count={egress_queue}")
    if queue_count and not worker_active:
        warnings.append(f"queue_count={queue_count} com worker inativo")

    health_status = "degraded" if issues else "warn" if warnings else "ok"
    return {
        "ok": True,
        "diagnostic": True,
        "healthy": not issues,
        "status": health_status,
        "issues": issues,
        "warnings": warnings,
        "format": fmt,
        "worker_active": worker_active,
        "worker_state": worker_state,
        "packet_drops": packet_drops,
        "opus_egress_packet_drops": egress_drops,
        "opus_egress_queue_count": egress_queue,
        "opus_codec_error": codec_error,
        "rollback_hint": "codec-v2 transport-disable ou NOISEBOT_AUDIO_DEFAULT_CODEC=pcm16",
    }


def _int_field(payload: dict[str, Any], key: str) -> int:
    try:
        return int(payload.get(key) or 0)
    except (TypeError, ValueError):
        return 0


__all__ = ["FirmwareDiagClient", "FirmwareDiagError", "codec_v2_health_from_status"]
