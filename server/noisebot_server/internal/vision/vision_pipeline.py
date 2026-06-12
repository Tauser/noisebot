"""Vision pipeline: presence-gated face detection with adaptive cadence.

State machine:
  IDLE    — no capture; observe() every 30 s to check motion/luma gate
  ACQUIRE — burst at 3 Hz for up to 5 s to find a face
  TRACK   — face confirmed; 2 Hz + EMA smoothing + dead-zone gaze updates
  LOST    — K consecutive misses; clears face box and returns to IDLE
"""
from __future__ import annotations

import asyncio
import logging
import time
from dataclasses import dataclass, field
from enum import Enum
from typing import TYPE_CHECKING, Any, Callable

from .analysis import analyze_jpeg, is_detector_available

if TYPE_CHECKING:
    from ..transport.adapter import FirmwareAdapter
    from .client import VisionClient, VisionObservation

log = logging.getLogger(__name__)

_ACQUIRE_INTERVAL_S: float = 1.0 / 3.0  # 3 Hz
_TRACK_INTERVAL_S: float = 0.5          # 2 Hz
_IDLE_CHECK_INTERVAL_S: float = 30.0    # opportunistic presence check

_ACQUIRE_TIMEOUT_S: float = 5.0
_CONFIRM_HITS: int = 2   # consecutive detections to enter TRACK
_MISS_THRESHOLD: int = 3  # consecutive misses to leave TRACK

_EMA_ALPHA: float = 0.4
_GAZE_DEADZONE: float = 0.05  # suppress gaze updates below this delta


class PipelineState(str, Enum):
    IDLE = "IDLE"
    ACQUIRE = "ACQUIRE"
    TRACK = "TRACK"
    LOST = "LOST"


@dataclass
class PipelineCounters:
    ticks_idle: int = 0
    ticks_acquire: int = 0
    ticks_track: int = 0
    ticks_lost: int = 0
    detections: int = 0
    gaze_sends: int = 0
    face_box_sends: int = 0
    capture_errors: int = 0
    send_errors: int = 0
    state_transitions: int = 0
    last_detection_ts: float | None = None


class VisionPipeline:
    """Presence-gated face detection with adaptive cadence.

    Replaces FaceLoop. Key differences:
    - Adapter pulled at each send (no injection task, no silent wiring failure).
    - Observation dimensions taken from real observe() call, not hardcoded 240.
    - Import/model errors surface at init_analyzer() boot time, not silently per tick.
    - State machine with hysteresis eliminates gaze flicker.
    - EMA + dead zone reduces redundant gaze messages.
    """

    def __init__(
        self,
        vision_client: "VisionClient",
        get_adapter: Callable[[], "FirmwareAdapter | None"],
    ) -> None:
        self._vision = vision_client
        self._get_adapter = get_adapter
        self._state = PipelineState.IDLE
        self._running = False
        self._task: asyncio.Task | None = None
        self._counters = PipelineCounters()

        self._consecutive_hits: int = 0
        self._consecutive_misses: int = 0
        self._acquire_start_ts: float = 0.0
        self._cached_obs: "VisionObservation | None" = None

        self._ema_x: float | None = None
        self._ema_y: float | None = None
        self._last_sent_x: float | None = None
        self._last_sent_y: float | None = None
        self._last_face: tuple[int, int, int, int] | None = None  # (x, y, w, h)

    @property
    def state(self) -> PipelineState:
        return self._state

    @property
    def counters(self) -> PipelineCounters:
        return self._counters

    def is_adapter_connected(self) -> bool:
        adapter = self._get_adapter()
        return adapter is not None and adapter.is_connected

    def start(self) -> None:
        if self._running:
            return
        self._running = True
        self._task = asyncio.ensure_future(self._run())
        log.info(
            "VisionPipeline iniciado (detector=%s)",
            "yunet" if is_detector_available() else "indisponivel",
        )

    def stop(self) -> None:
        self._running = False
        if self._task:
            self._task.cancel()
            self._task = None
        self._state = PipelineState.IDLE  # reset para que get_social_presence retorne NO_ONE
        self._consecutive_misses = 0
        self._consecutive_hits = 0
        log.info("VisionPipeline parado")

    async def stop_async(self) -> None:
        """Para o pipeline enviando clear_face_box antes de cancelar.

        Usar em vez de stop() quando o caller é async e o bridge pode estar
        conectado — garante que o firmware não fique com s_face_box_valid=true
        e que o dashboard volte a mostrar NO_ONE imediatamente.
        """
        if self._state == PipelineState.TRACK:
            await self._clear_face_box()
        self.stop()

    def status_dict(self) -> dict:
        c = self._counters
        fb = self._last_face
        return {
            "state": self._state.value,
            "running": self._running,
            "detector_available": is_detector_available(),
            "adapter_connected": self.is_adapter_connected(),
            "last_detection_ts": c.last_detection_ts,
            "detections": c.detections,
            "gaze_sends": c.gaze_sends,
            "face_box_sends": c.face_box_sends,
            "capture_errors": c.capture_errors,
            "send_errors": c.send_errors,
            "state_transitions": c.state_transitions,
            "last_face_box": {"x": fb[0], "y": fb[1], "w": fb[2], "h": fb[3]} if fb else None,
        }

    async def _run(self) -> None:
        while self._running:
            try:
                interval = await self._tick()
            except asyncio.CancelledError:
                break
            except Exception as exc:
                log.debug("VisionPipeline tick error: %s", exc)
                interval = _ACQUIRE_INTERVAL_S
            await asyncio.sleep(interval)

    async def _tick(self) -> float:
        if self._state == PipelineState.IDLE:
            return await self._tick_idle()
        if self._state == PipelineState.ACQUIRE:
            return await self._tick_acquire()
        if self._state == PipelineState.TRACK:
            return await self._tick_track()
        # LOST
        return await self._tick_lost()

    async def _tick_idle(self) -> float:
        self._counters.ticks_idle += 1
        obs = await asyncio.to_thread(self._safe_observe)
        if obs is not None:
            # only motion triggers acquisition; luma alone is not enough
            if obs.motion_score > 10:
                self._cached_obs = obs
                self._consecutive_hits = 0
                self._acquire_start_ts = time.monotonic()
                self._transition_to(PipelineState.ACQUIRE)
                return _ACQUIRE_INTERVAL_S
        return _IDLE_CHECK_INTERVAL_S

    async def _tick_acquire(self) -> float:
        self._counters.ticks_acquire += 1
        if time.monotonic() - self._acquire_start_ts > _ACQUIRE_TIMEOUT_S:
            log.info("VisionPipeline: ACQUIRE timeout — sem rosto")
            self._transition_to(PipelineState.IDLE)
            return _IDLE_CHECK_INTERVAL_S

        jpeg = await asyncio.to_thread(self._safe_capture)
        if not jpeg:
            return _ACQUIRE_INTERVAL_S

        obs = self._cached_obs or await asyncio.to_thread(self._safe_observe)
        if obs is None:
            return _ACQUIRE_INTERVAL_S
        self._cached_obs = obs

        analysis = await asyncio.to_thread(analyze_jpeg, jpeg, obs)
        if analysis.face_detected:
            self._consecutive_hits += 1
            if self._consecutive_hits >= _CONFIRM_HITS:
                self._consecutive_misses = 0
                self._counters.detections += 1
                self._counters.last_detection_ts = time.time()
                log.info(
                    "VisionPipeline: rosto confirmado (%d hits)", self._consecutive_hits
                )
                self._transition_to(PipelineState.TRACK)
                await self._emit_gaze(analysis)
        else:
            self._consecutive_hits = 0
        return _ACQUIRE_INTERVAL_S

    async def _tick_track(self) -> float:
        self._counters.ticks_track += 1

        jpeg = await asyncio.to_thread(self._safe_capture)
        if not jpeg:
            self._consecutive_misses += 1
            if self._consecutive_misses >= _MISS_THRESHOLD:
                log.info("VisionPipeline: rosto perdido (capture falhou %d×)", self._consecutive_misses)
                self._transition_to(PipelineState.LOST)
                await self._clear_face_box()
            return _TRACK_INTERVAL_S

        obs = self._cached_obs or await asyncio.to_thread(self._safe_observe)
        if obs is None:
            self._consecutive_misses += 1
            if self._consecutive_misses >= _MISS_THRESHOLD:
                log.info("VisionPipeline: rosto perdido (observe falhou %d×)", self._consecutive_misses)
                self._transition_to(PipelineState.LOST)
                await self._clear_face_box()
            return _TRACK_INTERVAL_S
        self._cached_obs = obs

        analysis = await asyncio.to_thread(analyze_jpeg, jpeg, obs)
        if analysis.face_detected and analysis.primary_face is not None:
            self._consecutive_misses = 0
            self._counters.detections += 1
            self._counters.last_detection_ts = time.time()
            await self._emit_gaze(analysis)
            await self._emit_face_box(analysis)
        else:
            self._consecutive_misses += 1
            if self._consecutive_misses >= _MISS_THRESHOLD:
                log.info(
                    "VisionPipeline: rosto perdido (%d misses)", self._consecutive_misses
                )
                self._transition_to(PipelineState.LOST)
                await self._clear_face_box()
        return _TRACK_INTERVAL_S

    async def _tick_lost(self) -> float:
        self._counters.ticks_lost += 1
        self._ema_x = None
        self._ema_y = None
        self._last_sent_x = None
        self._last_sent_y = None
        self._transition_to(PipelineState.IDLE)
        return _IDLE_CHECK_INTERVAL_S

    def _transition_to(self, new_state: PipelineState) -> None:
        if new_state != self._state:
            log.info(
                "VisionPipeline: %s → %s", self._state.value, new_state.value
            )
            self._state = new_state
            self._counters.state_transitions += 1

    def _safe_observe(self) -> "VisionObservation | None":
        try:
            return self._vision.observe()
        except Exception as exc:
            log.debug("VisionPipeline observe error: %s", exc)
            self._counters.capture_errors += 1
            return None

    def _safe_capture(self) -> bytes | None:
        try:
            return self._vision.snapshot()
        except Exception as exc:
            log.debug("VisionPipeline capture error: %s", exc)
            self._counters.capture_errors += 1
            return None

    async def _emit_gaze(self, analysis: Any) -> None:
        norm_x = analysis.face_center_norm_x
        norm_y = analysis.face_center_norm_y
        if norm_x is None or norm_y is None:
            return

        if self._ema_x is None:
            self._ema_x, self._ema_y = norm_x, norm_y
        else:
            self._ema_x = _EMA_ALPHA * norm_x + (1.0 - _EMA_ALPHA) * self._ema_x
            self._ema_y = _EMA_ALPHA * norm_y + (1.0 - _EMA_ALPHA) * self._ema_y  # type: ignore[operator]

        if (
            self._last_sent_x is not None
            and self._last_sent_y is not None
            and abs(self._ema_x - self._last_sent_x) < _GAZE_DEADZONE
            and abs(self._ema_y - self._last_sent_y) < _GAZE_DEADZONE  # type: ignore[operator]
        ):
            return

        adapter = self._get_adapter()
        if adapter is None or not adapter.is_connected:
            return
        try:
            await adapter.send_gaze(self._ema_x, self._ema_y)  # type: ignore[arg-type]
            self._last_sent_x = self._ema_x
            self._last_sent_y = self._ema_y
            self._counters.gaze_sends += 1
        except Exception as exc:
            log.debug("VisionPipeline send_gaze error: %s", exc)
            self._counters.send_errors += 1

    async def _emit_face_box(self, analysis: Any) -> None:
        face = analysis.primary_face
        if face is None:
            return
        adapter = self._get_adapter()
        if adapter is None or not adapter.is_connected:
            return
        try:
            await adapter.send_face_box(face.x, face.y, face.width, face.height)
            self._last_face = (face.x, face.y, face.width, face.height)
            self._counters.face_box_sends += 1
        except Exception as exc:
            log.debug("VisionPipeline send_face_box error: %s", exc)
            self._counters.send_errors += 1

    async def _clear_face_box(self) -> None:
        self._last_face = None
        adapter = self._get_adapter()
        if adapter is None or not adapter.is_connected:
            return
        try:
            await adapter.send_face_box(0, 0, 0, 0)
        except Exception as exc:
            log.debug("VisionPipeline clear_face_box error: %s", exc)


__all__ = ["VisionPipeline", "PipelineState", "PipelineCounters"]
