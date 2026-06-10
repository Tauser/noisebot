"""Local visual analysis over firmware JPEG snapshots."""

from __future__ import annotations

import base64
import json
import os
from dataclasses import dataclass
from typing import TYPE_CHECKING
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

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


_VISION_DESCRIBE_PROMPT = (
    "Voce e os olhos de um robo companion. Descreva em 2-3 frases curtas e diretas "
    "o que esta visivel nesta imagem: pessoas presentes (quantidade, o que estao fazendo), "
    "ambiente, objetos em destaque. Se a imagem estiver escura ou vazia, diga isso. "
    "Responda em portugues do Brasil."
)

_VISION_API_TIMEOUT_S = 8.0
_VISION_MAX_JPEG_BYTES = 1_000_000  # 1 MB — sane limit before base64


def describe_with_vision_api(
    jpeg_bytes: bytes,
    *,
    api_key: str | None = None,
    base_url: str = "https://api.openai.com/v1",
    model: str = "gpt-4o-mini",
) -> str | None:
    """Call an OpenAI-compatible vision API to describe the JPEG content.

    Returns a Portuguese description string, or None on any failure.
    Uses urllib so it is safe to call from a thread (no event loop required).
    """
    key = api_key or os.environ.get("OPENAI_API_KEY", "")
    if not key or not jpeg_bytes:
        return None
    if len(jpeg_bytes) > _VISION_MAX_JPEG_BYTES:
        return None

    b64 = base64.b64encode(jpeg_bytes).decode("ascii")
    body = {
        "model": model,
        "max_tokens": 150,
        "messages": [
            {
                "role": "user",
                "content": [
                    {"type": "text", "text": _VISION_DESCRIBE_PROMPT},
                    {
                        "type": "image_url",
                        "image_url": {"url": f"data:image/jpeg;base64,{b64}", "detail": "low"},
                    },
                ],
            }
        ],
    }
    payload = json.dumps(body).encode("utf-8")
    url = base_url.rstrip("/") + "/chat/completions"
    req = Request(
        url,
        data=payload,
        method="POST",
        headers={
            "Authorization": f"Bearer {key}",
            "Content-Type": "application/json",
        },
    )
    try:
        with urlopen(req, timeout=_VISION_API_TIMEOUT_S) as resp:
            data = json.loads(resp.read().decode("utf-8"))
        return str(data["choices"][0]["message"]["content"]).strip()
    except (HTTPError, URLError, TimeoutError, OSError, KeyError, json.JSONDecodeError):
        return None


__all__ = ["FaceBox", "VisionAnalysis", "analyze_jpeg", "describe_with_vision_api"]
