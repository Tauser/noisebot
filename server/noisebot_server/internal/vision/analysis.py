"""Local visual analysis over firmware JPEG snapshots."""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .client import VisionObservation


@dataclass(frozen=True)
class FaceBox:
    x: int
    y: int
    width: int
    height: int

    @property
    def center_x(self) -> float:
        return self.x + self.width / 2.0

    @property
    def center_y(self) -> float:
        return self.y + self.height / 2.0


@dataclass(frozen=True)
class VisionAnalysis:
    observation: "VisionObservation"
    detector: str
    detector_available: bool
    face_detected: bool
    face_count: int
    primary_face: FaceBox | None = None
    error: str | None = None

    @property
    def face_center_norm_x(self) -> float | None:
        if self.primary_face is None or self.observation.width <= 0:
            return None
        return (self.primary_face.center_x / float(self.observation.width)) * 2.0 - 1.0

    @property
    def face_center_norm_y(self) -> float | None:
        if self.primary_face is None or self.observation.height <= 0:
            return None
        return (self.primary_face.center_y / float(self.observation.height)) * 2.0 - 1.0


def analyze_jpeg(jpeg_bytes: bytes, observation: "VisionObservation") -> VisionAnalysis:
    try:
        import cv2  # type: ignore[import]
        import numpy as np  # type: ignore[import]
    except Exception as exc:
        return VisionAnalysis(
            observation=observation,
            detector="opencv_haar",
            detector_available=False,
            face_detected=False,
            face_count=0,
            error=f"opencv_unavailable:{exc.__class__.__name__}",
        )

    image_array = np.frombuffer(jpeg_bytes, dtype=np.uint8)
    image = cv2.imdecode(image_array, cv2.IMREAD_COLOR)
    if image is None:
        return VisionAnalysis(
            observation=observation,
            detector="opencv_haar",
            detector_available=True,
            face_detected=False,
            face_count=0,
            error="jpeg_decode_failed",
        )

    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    cascade_path = cv2.data.haarcascades + "haarcascade_frontalface_default.xml"
    cascade = cv2.CascadeClassifier(cascade_path)
    if cascade.empty():
        return VisionAnalysis(
            observation=observation,
            detector="opencv_haar",
            detector_available=False,
            face_detected=False,
            face_count=0,
            error="cascade_unavailable",
        )

    faces = cascade.detectMultiScale(
        gray,
        scaleFactor=1.1,
        minNeighbors=4,
        minSize=(36, 36),
        flags=cv2.CASCADE_SCALE_IMAGE,
    )
    boxes = [FaceBox(int(x), int(y), int(w), int(h)) for (x, y, w, h) in faces]
    primary = max(boxes, key=lambda box: box.width * box.height) if boxes else None
    return VisionAnalysis(
        observation=observation,
        detector="opencv_haar",
        detector_available=True,
        face_detected=primary is not None,
        face_count=len(boxes),
        primary_face=primary,
    )


__all__ = ["FaceBox", "VisionAnalysis", "analyze_jpeg"]
