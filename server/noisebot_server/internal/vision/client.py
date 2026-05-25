"""HTTP client for firmware vision endpoints."""

from __future__ import annotations

import json
import os
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
        try:
            with urlopen(request, timeout=self.timeout_s) as response:
                return response.read()
        except (HTTPError, URLError, TimeoutError, OSError) as exc:
            raise VisionError(str(exc)) from exc


def _env_float(key: str, default: float) -> float:
    try:
        return float(os.environ.get(key, default))
    except (TypeError, ValueError):
        return default


__all__ = ["VisionClient", "VisionError", "VisionObservation"]
