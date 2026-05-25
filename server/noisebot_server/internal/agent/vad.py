"""Secondary VAD used for barge-in detection."""

from __future__ import annotations

import logging
import math
import struct

log = logging.getLogger(__name__)

DEFAULT_THRESHOLD_RMS: float = 200.0
DEFAULT_SUSTAIN_CHUNKS: int = 10


def _compute_rms(pcm: bytes) -> float:
    n = len(pcm) // 2
    if n == 0:
        return 0.0
    samples = struct.unpack_from(f"<{n}h", pcm)
    sq_sum = sum(sample * sample for sample in samples)
    return math.sqrt(sq_sum / n)


class BargeInMonitor:
    """Detect sustained speech energy while the robot is speaking."""

    def __init__(
        self,
        threshold_rms: float = DEFAULT_THRESHOLD_RMS,
        sustain_chunks: int = DEFAULT_SUSTAIN_CHUNKS,
    ) -> None:
        self._threshold = threshold_rms
        self._sustain = sustain_chunks
        self._above_count = 0

    @property
    def above_count(self) -> int:
        return self._above_count

    def feed(self, pcm: bytes) -> bool:
        rms = _compute_rms(pcm)
        if rms >= self._threshold:
            self._above_count += 1
            log.debug(
                "VAD barge-in: rms=%.0f above_count=%d/%d",
                rms,
                self._above_count,
                self._sustain,
            )
        else:
            self._above_count = 0
        return self._above_count >= self._sustain

    def reset(self) -> None:
        self._above_count = 0


__all__ = [
    "BargeInMonitor",
    "DEFAULT_SUSTAIN_CHUNKS",
    "DEFAULT_THRESHOLD_RMS",
]
