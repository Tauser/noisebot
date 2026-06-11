"""Vision internals — face detection and gaze only."""

from __future__ import annotations

from .analysis import (
    FaceBox,
    VisionAnalysis,
    analyze_jpeg,
    init_analyzer,
    is_detector_available,
)
from .client import VisionClient, VisionError, VisionObservation
from .face_loop import FaceLoop  # kept for reference; use VisionPipeline for new code
from .vision_pipeline import PipelineState, VisionPipeline

__all__ = [
    "FaceBox",
    "FaceLoop",
    "PipelineState",
    "VisionAnalysis",
    "VisionClient",
    "VisionError",
    "VisionObservation",
    "VisionPipeline",
    "analyze_jpeg",
    "init_analyzer",
    "is_detector_available",
]
