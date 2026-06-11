"""HTTP client for firmware vision endpoints."""

from __future__ import annotations

import json
import os
import threading
from dataclasses import dataclass
from urllib.error import HTTPError, URLError
from urllib.parse import urljoin
from urllib.request import Request, urlopen

from .analysis import VisionAnalysis, analyze_jpeg


class VisionError(RuntimeError):
    """Firmware vision query failed."""


@dataclass(frozen=True)
class VisionObservation:
    valid: bool
    scene: str
    timestamp_ms: int
    width: int
    height: int
    jpeg_bytes: int
    capture_ms: int
    luma_avg: int
    luma_min: int
    luma_max: int
    contrast: int
    motion_score: int

    @classmethod
    def from_payload(cls, payload: dict) -> "VisionObservation":
        observation = payload.get("observation", payload)
        if not isinstance(observation, dict):
            raise VisionError("payload de visao invalido")
        return cls(
            valid=bool(observation.get("valid", False)),
            scene=str(observation.get("scene", "unknown")),
            timestamp_ms=int(observation.get("timestamp_ms", 0) or 0),
            width=int(observation.get("width", 0) or 0),
            height=int(observation.get("height", 0) or 0),
            jpeg_bytes=int(observation.get("jpeg_bytes", 0) or 0),
            capture_ms=int(observation.get("capture_ms", 0) or 0),
            luma_avg=int(observation.get("luma_avg", 0) or 0),
            luma_min=int(observation.get("luma_min", 0) or 0),
            luma_max=int(observation.get("luma_max", 0) or 0),
            contrast=int(observation.get("contrast", 0) or 0),
            motion_score=int(observation.get("motion_score", 0) or 0),
        )


class VisionClient:
    """Small HTTP client for `/api/vision/observe` and camera snapshots."""

    def __init__(self, base_url: str, timeout_s: float = 3.0) -> None:
        self.base_url = base_url.rstrip("/") + "/"
        self.timeout_s = timeout_s
        self._lock = threading.Lock()

    @classmethod
    def from_config(cls, config) -> "VisionClient | None":
        explicit = os.environ.get("NOISEBOT_ROBOT_HTTP_URL", "").strip()
        if explicit:
            return cls(explicit, _env_float("NOISEBOT_VISION_TIMEOUT_S", 3.0))

        host = getattr(getattr(config, "transport", None), "host", None)
        if not host:
            return None
        return cls(f"http://{host}", _env_float("NOISEBOT_VISION_TIMEOUT_S", 3.0))

    def observe(self) -> VisionObservation:
        payload = self._get_json("/api/vision/observe")
        ok = bool(payload.get("ok", True))
        if not ok:
            raise VisionError(str(payload.get("error", "visao indisponivel")))
        return VisionObservation.from_payload(payload)

    def snapshot(self) -> bytes:
        return self._get_bytes("/api/camera/snapshot")

    def set_mode(self, mode: str) -> None:
        """Set firmware camera mode ('safe' or 'better').

        Resolution is native/effective and fixed by the firmware's compiled
        CONFIG_CAMERA_OV2640_DVP_* Kconfig choice regardless of mode — the
        OV2640 DVP path rejects any other size via VIDIOC_S_FMT/TRY_FMT
        (errno=22). Mode only selects the memory-threshold profile checked
        before HAL init. Check /api/camera/status's effective_width/height
        for the resolution actually active on a given build (it varies by
        which Kconfig mode was flashed — see docs/CAMERA_INTEGRATION.md
        "Experimento de alta resolucao (640x480)").
        """
        self._post_json("/api/camera/mode", {"mode": mode})

    def snapshot_and_close(self) -> bytes:
        """Capture a single JPEG and immediately release the camera session."""
        jpeg = self._get_bytes("/api/camera/snapshot")
        try:
            self._post_json("/api/camera/session/close", {})
        except VisionError:
            pass
        return jpeg


    def session_close(self) -> None:
        """Release the firmware camera session (allow it to power down)."""
        try:
            self._post_json("/api/camera/session/close", {})
        except VisionError:
            pass

    def poll_stop(self) -> None:
        """Pause firmware vision presence poll during explicit vision work."""
        self._post_json("/api/vision/poll/stop", {})

    def poll_start(self, interval_ms: int = 0) -> None:
        """Resume firmware vision presence poll."""
        body: dict = {}
        if interval_ms > 0:
            body["interval_ms"] = interval_ms
        self._post_json("/api/vision/poll/start", body)

    def get_lightweight_snapshot(self) -> "dict | None":
        """Return a compact vision dict for the LLM payload.

        Uses /api/vision/observe only — no JPEG capture, no face detection.
        Returns None silently on any failure so vision never blocks the pipeline.
        """
        try:
            obs = self.observe()
        except Exception:
            return None
        return {
            "available": True,
            "fresh": True,
            "scene": obs.scene,
            "brightness": _luma_to_text(obs.luma_avg),
            "motion": _motion_to_text(obs.motion_score),
        }

    def analyze(self) -> VisionAnalysis:
        observation = self.observe()
        jpeg = self.snapshot()
        return analyze_jpeg(jpeg, observation)

    def _get_json(self, path: str) -> dict:
        data = self._get_bytes(path).decode("utf-8")
        try:
            payload = json.loads(data)
        except json.JSONDecodeError as exc:
            raise VisionError("resposta de visao nao e JSON") from exc
        if not isinstance(payload, dict):
            raise VisionError("resposta de visao invalida")
        return payload

    def _get_bytes(self, path: str) -> bytes:
        url = urljoin(self.base_url, path.lstrip("/"))
        request = Request(url, headers={"User-Agent": "NoiseBot-Server/0.1"})
        with self._lock:
            try:
                with urlopen(request, timeout=self.timeout_s) as response:
                    return response.read()
            except (HTTPError, URLError, TimeoutError, OSError) as exc:
                raise VisionError(str(exc)) from exc

    def _post_json(self, path: str, body: dict) -> None:
        url = urljoin(self.base_url, path.lstrip("/"))
        data = json.dumps(body).encode("utf-8")
        req = Request(
            url,
            data=data,
            method="POST",
            headers={
                "Content-Type": "application/json",
                "User-Agent": "NoiseBot-Server/0.1",
            },
        )
        with self._lock:
            try:
                with urlopen(req, timeout=self.timeout_s) as response:
                    response.read()
            except (HTTPError, URLError, TimeoutError, OSError) as exc:
                raise VisionError(str(exc)) from exc


def _luma_to_text(luma: int) -> str:
    if luma < 60:
        return "baixa"
    if luma < 150:
        return "média"
    return "alta"


def _motion_to_text(motion_score: int) -> str:
    if motion_score < 10:
        return "baixo"
    if motion_score < 50:
        return "moderado"
    return "alto"


def _env_float(key: str, default: float) -> float:
    try:
        return float(os.environ.get(key, default))
    except (TypeError, ValueError):
        return default


__all__ = ["VisionClient", "VisionError", "VisionObservation"]
