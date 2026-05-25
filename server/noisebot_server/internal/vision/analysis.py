"""Vision analysis facade."""

from __future__ import annotations

from ..._compat import ensure_bridgev2_path

ensure_bridgev2_path()

from bridgev2.vision.analyzer import FaceBox, VisionAnalysis, analyze_jpeg

__all__ = [
    "FaceBox",
    "VisionAnalysis",
    "analyze_jpeg",
]
