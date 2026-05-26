"""HTTP client for firmware agenda endpoints."""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from urllib.error import HTTPError, URLError
from urllib.parse import urljoin
from urllib.request import Request, urlopen


class FirmwareAgendaError(RuntimeError):
    """Firmware agenda query failed."""


@dataclass(frozen=True)
class FirmwareAgendaClient:
    """Small HTTP client for `/api/agenda` on the robot firmware."""

    base_url: str
    timeout_s: float = 1.5

    @classmethod
    def from_config(cls, config) -> "FirmwareAgendaClient | None":
        explicit = os.environ.get("NOISEBOT_ROBOT_HTTP_URL", "").strip()
        if explicit:
            return cls(explicit.rstrip("/") + "/", _env_float("NOISEBOT_AGENDA_TIMEOUT_S", 1.5))

        host = getattr(getattr(config, "transport", None), "host", None)
        if not host:
            return None
        return cls(f"http://{host}/", _env_float("NOISEBOT_AGENDA_TIMEOUT_S", 1.5))

    def fetch(self) -> dict:
        url = urljoin(self.base_url, "api/agenda")
        request = Request(url, headers={"User-Agent": "NoiseBot-Server/0.1"})
        try:
            with urlopen(request, timeout=self.timeout_s) as response:
                data = response.read().decode("utf-8")
        except (HTTPError, URLError, TimeoutError, OSError) as exc:
            raise FirmwareAgendaError(str(exc)) from exc

        try:
            payload = json.loads(data)
        except json.JSONDecodeError as exc:
            raise FirmwareAgendaError("resposta de agenda nao e JSON") from exc
        if not isinstance(payload, dict):
            raise FirmwareAgendaError("resposta de agenda invalida")
        return payload


def _env_float(key: str, default: float) -> float:
    try:
        return float(os.environ.get(key, default))
    except (TypeError, ValueError):
        return default


__all__ = ["FirmwareAgendaClient", "FirmwareAgendaError"]
