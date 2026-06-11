"""Face detection loop — presence and gaze only.

Every ~POLL_INTERVAL_S:
  1. Capture JPEG from firmware camera
  2. Haar Cascade → fast face check
  3. If face: send MSG_GAZE(norm_x, norm_y) + MSG_FACE_BOX to firmware
"""
from __future__ import annotations

import asyncio
import logging
from typing import TYPE_CHECKING

from .analysis import analyze_jpeg, FaceBox

if TYPE_CHECKING:
    from ..transport.adapter import FirmwareAdapter
    from .client import VisionClient, VisionObservation

log = logging.getLogger(__name__)

POLL_INTERVAL_S: float = 2.0


def _norm_coord(center: float, dim: int) -> float:
    if dim <= 0:
        return 0.0
    return (center / float(dim)) * 2.0 - 1.0


class FaceLoop:
    """Detects faces and sends gaze/face-box commands to firmware."""

    def __init__(
        self,
        vision_client: "VisionClient",
    ) -> None:
        self._vision = vision_client
        self._adapter: "FirmwareAdapter | None" = None
        self._running = False
        self._task: asyncio.Task | None = None

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

        observation = _dummy_observation()
        analysis = await asyncio.to_thread(analyze_jpeg, jpeg, observation)

        if not analysis.face_detected or analysis.primary_face is None:
            return

        face = analysis.primary_face
        norm_x = _norm_coord(face.center_x, 240)
        norm_y = _norm_coord(face.center_y, 240)
        await self._send_gaze(norm_x, norm_y)
        await self._send_face_box(face.x, face.y, face.width, face.height)

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

    def _capture(self) -> bytes | None:
        try:
            return self._vision.snapshot()
        except Exception as exc:
            log.debug("FaceLoop capture error: %s", exc)
            return None


def _dummy_observation() -> "VisionObservation":
    from .client import VisionObservation
    return VisionObservation(
        valid=True, scene="unknown", timestamp_ms=0,
        width=240, height=240, jpeg_bytes=0,
        capture_ms=0, luma_avg=0, luma_min=0, luma_max=0,
        contrast=0, motion_score=0,
    )


__all__ = ["FaceLoop", "POLL_INTERVAL_S"]
