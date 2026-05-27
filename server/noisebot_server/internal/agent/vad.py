"""Secondary VAD used for barge-in detection."""

from __future__ import annotations

import logging
import math
import struct

log = logging.getLogger(__name__)

DEFAULT_THRESHOLD_RMS: float = 1200.0
DEFAULT_SUSTAIN_CHUNKS: int = 8
DEFAULT_BASELINE_RATIO: float = 1.9
DEFAULT_BASELINE_MARGIN: float = 900.0


def _compute_rms(pcm: bytes) -> float:
    n = len(pcm) // 2
    if n == 0:
        return 0.0
    samples = struct.unpack_from(f"<{n}h", pcm)
    sq_sum = sum(sample * sample for sample in samples)
    return math.sqrt(sq_sum / n)


class BargeInMonitor:
    """Detect sustained near-field speech while the robot is speaking.

    The firmware streams mic chunks during playback, so the monitor first learns
    the acoustic floor of the robot's own TTS leakage. Barge-in only fires when
    the incoming mic energy rises clearly above that moving floor.
    """

    def __init__(
        self,
        threshold_rms: float = DEFAULT_THRESHOLD_RMS,
        sustain_chunks: int = DEFAULT_SUSTAIN_CHUNKS,
        baseline_ratio: float = DEFAULT_BASELINE_RATIO,
        baseline_margin: float = DEFAULT_BASELINE_MARGIN,
    ) -> None:
        self._threshold = threshold_rms
        self._sustain = sustain_chunks
        self._baseline_ratio = baseline_ratio
        self._baseline_margin = baseline_margin
        self._baseline: float | None = None
        self._above_count = 0

    @property
    def above_count(self) -> int:
        return self._above_count

    @property
    def baseline(self) -> float | None:
        return self._baseline

    def feed(self, pcm: bytes, *, allow_trigger: bool = True) -> bool:
        rms = _compute_rms(pcm)
        if self._baseline is None:
            self._baseline = rms
            return False

        dynamic_threshold = max(
            self._threshold,
            self._baseline * self._baseline_ratio,
            self._baseline + self._baseline_margin,
        )
        if not allow_trigger:
            self._above_count = 0
            self._baseline = (self._baseline * 0.95) + (rms * 0.05)
        elif rms >= dynamic_threshold:
            self._above_count += 1
            log.debug(
                "VAD barge-in: rms=%.0f threshold=%.0f baseline=%.0f above_count=%d/%d",
                rms,
                dynamic_threshold,
                self._baseline,
                self._above_count,
                self._sustain,
            )
        else:
            self._above_count = 0
            self._baseline = (self._baseline * 0.98) + (rms * 0.02)
        return self._above_count >= self._sustain

    def reset(self) -> None:
        self._above_count = 0
        self._baseline = None


__all__ = [
    "BargeInMonitor",
    "DEFAULT_BASELINE_MARGIN",
    "DEFAULT_BASELINE_RATIO",
    "DEFAULT_SUSTAIN_CHUNKS",
    "DEFAULT_THRESHOLD_RMS",
]
