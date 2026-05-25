"""Firmware vision client facade."""

from __future__ import annotations

from ..._compat import ensure_bridgev2_path

ensure_bridgev2_path()

from bridgev2.vision.client import VisionClient, VisionError, VisionObservation

__all__ = [
    "VisionClient",
    "VisionError",
    "VisionObservation",
]
