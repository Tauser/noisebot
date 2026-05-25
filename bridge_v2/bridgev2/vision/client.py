"""bridgev2.vision.client -- Consulta HTTP leve aos endpoints de visao do firmware."""
from __future__ import annotations

from dataclasses import dataclass
import json
import os
from urllib.error import HTTPError, URLError
from urllib.parse import urljoin
from urllib.request import Request, urlopen


class VisionError(RuntimeError):
    """Falha ao consultar a visao do firmware."""


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
        obs = payload.get("observation", payload)
        if not isinstance(obs, dict):
            raise VisionError("payload de visao invalido")
        return cls(
            valid=bool(obs.get("valid", False)),
            scene=str(obs.get("scene", "unknown")),
            timestamp_ms=int(obs.get("timestamp_ms", 0) or 0),
            width=int(obs.get("width", 0) or 0),
            height=int(obs.get("height", 0) or 0),
            jpeg_bytes=int(obs.get("jpeg_bytes", 0) or 0),
            capture_ms=int(obs.get("capture_ms", 0) or 0),
            luma_avg=int(obs.get("luma_avg", 0) or 0),
            luma_min=int(obs.get("luma_min", 0) or 0),
            luma_max=int(obs.get("luma_max", 0) or 0),
            contrast=int(obs.get("contrast", 0) or 0),
            motion_score=int(obs.get("motion_score", 0) or 0),
        )


class VisionClient:
    """Cliente HTTP pequeno para `/api/vision/observe`.

    O bridge conversa com o firmware por TCP na porta 9000, mas os endpoints
    operacionais da camera ficam no HTTP local do robô.
    """

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

    def _get_json(self, path: str) -> dict:
        url = urljoin(self.base_url, path.lstrip("/"))
        request = Request(url, headers={"User-Agent": "NoiseBot-BridgeV2/2.0"})
        try:
            with urlopen(request, timeout=self.timeout_s) as response:
                data = response.read().decode("utf-8")
        except (HTTPError, URLError, TimeoutError, OSError) as exc:
            raise VisionError(str(exc)) from exc
        try:
            payload = json.loads(data)
        except json.JSONDecodeError as exc:
            raise VisionError("resposta de visao nao e JSON") from exc
        if not isinstance(payload, dict):
            raise VisionError("resposta de visao invalida")
        return payload


def _env_float(key: str, default: float) -> float:
    try:
        return float(os.environ.get(key, default))
    except (TypeError, ValueError):
        return default
