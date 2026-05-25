"""Vision orchestration internals.

Heavy visual analysis belongs on the server, not inside ESP32 firmware.
"""

from __future__ import annotations

from .analysis import FaceBox, VisionAnalysis, analyze_jpeg
from .client import VisionClient, VisionError, VisionObservation

__all__ = [
    "FaceBox",
    "VisionAnalysis",
    "VisionClient",
    "VisionError",
    "VisionObservation",
    "analyze_jpeg",
]
