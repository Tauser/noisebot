"""Cache de perfil de usuario (firmware) e snapshot leve de visao.

Extraido de orchestrator.py (SF-04, docs/ANALISE_SERVER_FINDINGS_2026-06-11.md):
ambos os caches fazem I/O bloqueante (firmware diag / vision HTTP) e por isso
sao chamados via `asyncio.to_thread` (ver SF-01).
"""

from __future__ import annotations

import asyncio
import logging
import time
from typing import Any

from ..ops.firmware_diag import FirmwareDiagClient, FirmwareDiagError
from ..vision import VisionClient

log = logging.getLogger(__name__)


def _clean_context_text(value: Any, limit: int) -> str:
    if value is None:
        return ""
    return " ".join(str(value).split())[:limit]


def _clean_user_profile(user: dict[str, Any]) -> dict[str, Any]:
    fields = {
        "id": 16,
        "display_name": 32,
        "relationship": 16,
        "language": 8,
        "robot_nickname": 24,
        "persona_mode": 24,
        "interaction_style": 24,
    }
    return {
        key: _clean_context_text(user.get(key), limit)
        for key, limit in fields.items()
        if _clean_context_text(user.get(key), limit)
    }


class PersonaSync:
    """Cacheia o perfil de usuario do firmware e o snapshot leve de visao."""

    _USER_PROFILE_TTL_S = 30.0
    _VISION_SNAPSHOT_TTL_S = 5.0

    def __init__(self, firmware_persona: FirmwareDiagClient | None) -> None:
        self._firmware_persona = firmware_persona
        self._user_profile_cache: dict[str, Any] | None = None
        self._user_profile_cache_at = 0.0
        self._vision_snapshot_cache: dict | None = None
        self._vision_snapshot_cache_at = 0.0

    async def current_user_profile(self) -> dict[str, Any] | None:
        now = time.monotonic()
        if (
            self._user_profile_cache is not None
            and now - self._user_profile_cache_at < self._USER_PROFILE_TTL_S
        ):
            return dict(self._user_profile_cache)
        if self._firmware_persona is None:
            return None
        try:
            payload = await asyncio.to_thread(self._firmware_persona.persona)
        except FirmwareDiagError as exc:
            log.debug("Perfil de usuario indisponivel no firmware: %s", exc)
            return dict(self._user_profile_cache) if self._user_profile_cache else None

        user = payload.get("user")
        if not isinstance(user, dict):
            return dict(self._user_profile_cache) if self._user_profile_cache else None
        self._user_profile_cache = _clean_user_profile(user)
        self._user_profile_cache_at = now
        return dict(self._user_profile_cache)

    async def get_vision_snapshot(self, vision: VisionClient | None) -> dict | None:
        """Return a cached lightweight vision snapshot, refreshing when stale.

        Returns None silently if `vision` is absent or the HTTP call fails.
        Stale cache is returned on failure to avoid losing context on transient errors.
        """
        if vision is None:
            return None
        now = time.monotonic()
        if (
            self._vision_snapshot_cache is not None
            and now - self._vision_snapshot_cache_at < self._VISION_SNAPSHOT_TTL_S
        ):
            return self._vision_snapshot_cache
        try:
            snapshot = await asyncio.to_thread(vision.get_lightweight_snapshot)
        except Exception as exc:
            log.debug("Vision snapshot falhou: %s", exc)
            return self._vision_snapshot_cache
        if snapshot is not None:
            self._vision_snapshot_cache = snapshot
            self._vision_snapshot_cache_at = now
        return snapshot
