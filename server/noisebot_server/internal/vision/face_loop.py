"""Async face detection / identification loop.

Every ~POLL_INTERVAL_S:
  1. Capture JPEG from firmware camera
  2. Haar Cascade → fast face check
  3. If face: Ollama identify → user_id or None
  4. Send MSG_GAZE(norm_x, norm_y) + MSG_FACE_BOX to firmware
  5. If user recognized for first time → activate persona profile + HAPPY expr
"""
from __future__ import annotations

import asyncio
import logging
import time
from typing import TYPE_CHECKING, Callable

from .face_service import FaceService, IdentifyResult

if TYPE_CHECKING:
    from ..transport.adapter import FirmwareAdapter
    from .client import VisionClient

log = logging.getLogger(__name__)

POLL_INTERVAL_S: float = 2.0
FACE_LOST_TIMEOUT_S: float = 5.0
AWAY_SLEEP_TIMEOUT_S: float = 60.0   # sem rosto por 60s → dorme

_EXPR_NEUTRAL = 0
_EXPR_HAPPY = 1
_EXPR_CURIOUS = 2

# Emotion events (mirrors firmware emotion_model.h)
_EMOT_ENTERING_SLEEP = 4
_EMOT_WAKING_UP = 5


def _norm_coord(center: float, dim: int) -> float:
    if dim <= 0:
        return 0.0
    return (center / float(dim)) * 2.0 - 1.0


class FaceLoop:
    """Detects faces, identifies users, and sends gaze/expression commands to firmware."""

    def __init__(
        self,
        face_service: FaceService,
        vision_client: "VisionClient",
        on_user_identified: Callable[[str, str], None] | None = None,
    ) -> None:
        self._svc = face_service
        self._vision = vision_client
        self._on_user_identified = on_user_identified
        self._adapter: "FirmwareAdapter | None" = None

        self._running = False
        self._task: asyncio.Task | None = None
        self._last_face_ts: float = 0.0
        self._last_user_id: str | None = None
        self._sleeping: bool = False

    # ── Public API ──────────────────────────────────────────────────────────

    def set_adapter(self, adapter: "FirmwareAdapter") -> None:
        self._adapter = adapter

    def start(self) -> None:
        if self._running:
            return
        self._running = True
        self._task = asyncio.ensure_future(self._run())
        log.info("FaceLoop started (poll=%.1fs)", POLL_INTERVAL_S)

    def stop(self) -> None:
        self._running = False
        if self._task:
            self._task.cancel()
            self._task = None
        log.info("FaceLoop stopped")

    # ── Main loop ───────────────────────────────────────────────────────────

    async def _run(self) -> None:
        while self._running:
            try:
                await self._tick()
            except asyncio.CancelledError:
                break
            except Exception as exc:
                log.debug("FaceLoop tick error: %s", exc)
            await asyncio.sleep(POLL_INTERVAL_S)

    async def _tick(self) -> None:
        jpeg = await asyncio.to_thread(self._capture)
        if not jpeg:
            return

        result: IdentifyResult = await asyncio.to_thread(self._svc.identify, jpeg)

        if result.face_box is None:
            if self._last_face_ts and (time.monotonic() - self._last_face_ts) > FACE_LOST_TIMEOUT_S:
                self._last_user_id = None
                # Away detection: se ausente por muito tempo → dorme
                if not self._sleeping and (time.monotonic() - self._last_face_ts) > AWAY_SLEEP_TIMEOUT_S:
                    self._sleeping = True
                    await self._send_emot_event(_EMOT_ENTERING_SLEEP)
                    log.info("FaceLoop: ausência prolongada → ENTERING_SLEEP")
            return

        self._last_face_ts = time.monotonic()

        # Acordar se estava dormindo
        if self._sleeping:
            self._sleeping = False
            await self._send_emot_event(_EMOT_WAKING_UP)
            log.info("FaceLoop: rosto detectado → WAKING_UP")

        # Gaze: map face center to normalized -1..+1 coords
        norm_x = _norm_coord(result.face_box.center_x, 320)
        norm_y = _norm_coord(result.face_box.center_y, 240)
        await self._send_gaze(norm_x, norm_y)

        # Face box overlay for firmware display
        await self._send_face_box(
            result.face_box.x, result.face_box.y,
            result.face_box.width, result.face_box.height,
        )

        # User identification
        if result.user_id and result.user_id != self._last_user_id:
            self._last_user_id = result.user_id
            await self._send_expr(_EXPR_HAPPY)
            if self._on_user_identified:
                try:
                    self._on_user_identified(result.user_id, result.display_name or result.user_id)
                except Exception as exc:
                    log.debug("on_user_identified callback error: %s", exc)
            log.info("User recognized: %s (%s)", result.user_id, result.display_name)

        elif result.user_id is None and not self._last_user_id:
            # Unknown face — show curiosity once per encounter
            if (time.monotonic() - self._last_face_ts) < POLL_INTERVAL_S * 1.5:
                await self._send_expr(_EXPR_CURIOUS)

    # ── Transport helpers ────────────────────────────────────────────────────

    async def _send_gaze(self, x: float, y: float) -> None:
        if self._adapter is None or not self._adapter.is_connected:
            return
        try:
            await self._adapter.send_gaze(x, y)
        except Exception as exc:
            log.debug("FaceLoop send_gaze error: %s", exc)

    async def _send_face_box(self, x: int, y: int, w: int, h: int) -> None:
        if self._adapter is None or not self._adapter.is_connected:
            return
        try:
            await self._adapter.send_face_box(x, y, w, h)
        except Exception as exc:
            log.debug("FaceLoop send_face_box error: %s", exc)

    async def _send_expr(self, expr_id: int, duration_ms: int = 1500) -> None:
        if self._adapter is None or not self._adapter.is_connected:
            return
        try:
            await self._adapter.send_expr(expr_id, duration_ms)
        except Exception as exc:
            log.debug("FaceLoop send_expr error: %s", exc)

    async def _send_emot_event(self, event_id: int) -> None:
        if self._adapter is None or not self._adapter.is_connected:
            return
        try:
            await self._adapter.send_emot_event(event_id)
        except Exception as exc:
            log.debug("FaceLoop send_emot_event error: %s", exc)

    # ── Camera helper ────────────────────────────────────────────────────────

    def _capture(self) -> bytes | None:
        try:
            return self._vision.snapshot()
        except Exception as exc:
            log.debug("FaceLoop capture error: %s", exc)
            return None


__all__ = ["FaceLoop", "POLL_INTERVAL_S"]
