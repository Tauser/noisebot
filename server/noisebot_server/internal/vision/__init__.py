"""Vision orchestration internals.

Heavy visual analysis belongs on the server, not inside ESP32 firmware.
"""

from __future__ import annotations

from .analysis import FaceBox, VisionAnalysis, analyze_jpeg
from .client import VisionClient, VisionError, VisionObservation
from .face_loop import FaceLoop
from .face_service import FaceService, IdentifyResult
from .face_store import FaceStore

__all__ = [
    "FaceBox",
    "FaceLoop",
    "FaceService",
    "FaceStore",
    "IdentifyResult",
    "VisionAnalysis",
    "VisionClient",
    "VisionError",
    "VisionObservation",
    "analyze_jpeg",
]
