"""Vision internals — face detection and gaze only."""

from __future__ import annotations

from .analysis import FaceBox, VisionAnalysis, analyze_jpeg
from .client import VisionClient, VisionError, VisionObservation
from .face_loop import FaceLoop

__all__ = [
    "FaceBox",
    "FaceLoop",
    "VisionAnalysis",
    "VisionClient",
    "VisionError",
    "VisionObservation",
    "analyze_jpeg",
]
