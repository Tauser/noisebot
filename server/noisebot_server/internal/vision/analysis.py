"""Local visual analysis over firmware JPEG snapshots."""

from __future__ import annotations

import base64
import json
import logging
import os
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from .client import VisionObservation

log = logging.getLogger(__name__)

_YUNET_MODEL_FILENAME = "face_detection_yunet_2023mar.onnx"
_DEFAULT_MODEL_DIR = Path(__file__).parent.parent.parent / "resource" / "models"

# Loaded once by init_analyzer(); None means detector is unavailable.
_detector: Any = None


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


def init_analyzer(model_path: Path | None = None) -> None:
    """Load YuNet detector once at startup. Raises RuntimeError on any failure.

    Call this early (e.g., in NoiseBotServer.run()) when NOISEBOT_VISION=1 so
    that a missing model or opencv import fails loudly at boot, not silently
    per frame.
    """
    global _detector
    try:
        import cv2  # type: ignore[import]
    except ImportError as exc:
        raise RuntimeError(
            "opencv indisponível — instale com: pip install -e .[vision]"
        ) from exc

    path = model_path or _default_model_path()
    if not path.exists():
        raise RuntimeError(
            f"Modelo YuNet não encontrado em '{path}'. "
            "Baixe 'face_detection_yunet_2023mar.onnx' de "
            "https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet "
            "e coloque em server/noisebot_server/resource/models/"
        )

    det = cv2.FaceDetectorYN.create(
        str(path),
        "",
        (240, 240),
        score_threshold=0.6,
        nms_threshold=0.3,
        top_k=5,
    )
    _detector = det
    log.info("YuNet inicializado: %s", path.name)


def is_detector_available() -> bool:
    return _detector is not None


def _default_model_path() -> Path:
    env = os.environ.get("NOISEBOT_YUNET_MODEL_PATH", "").strip()
    if env:
        return Path(env)
    return _DEFAULT_MODEL_DIR / _YUNET_MODEL_FILENAME


def analyze_jpeg(jpeg_bytes: bytes, observation: "VisionObservation") -> "VisionAnalysis":
    if _detector is None:
        return VisionAnalysis(
            observation=observation,
            detector="yunet",
            detector_available=False,
            face_detected=False,
            face_count=0,
            error="detector_not_initialized",
        )

    try:
        import cv2  # type: ignore[import]
        import numpy as np  # type: ignore[import]
    except Exception as exc:
        return VisionAnalysis(
            observation=observation,
            detector="yunet",
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
            detector="yunet",
            detector_available=True,
            face_detected=False,
            face_count=0,
            error="jpeg_decode_failed",
        )

    h, w = image.shape[:2]
    _detector.setInputSize((w, h))
    _, faces = _detector.detect(image)

    boxes: list[FaceBox] = []
    if faces is not None:
        for face in faces:
            fx, fy, fw, fh = int(face[0]), int(face[1]), int(face[2]), int(face[3])
            if fw > 0 and fh > 0:
                boxes.append(FaceBox(fx, fy, fw, fh))

    primary = max(boxes, key=lambda b: b.width * b.height) if boxes else None
    return VisionAnalysis(
        observation=observation,
        detector="yunet",
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
_VISION_MAX_JPEG_BYTES = 1_000_000


def describe_with_vision_api(
    jpeg_bytes: bytes,
    *,
    api_key: str | None = None,
    base_url: str = "https://api.openai.com/v1",
    model: str = "gpt-4o-mini",
) -> str | None:
    """Call an OpenAI-compatible vision API to describe the JPEG content."""
    from urllib.error import HTTPError, URLError
    from urllib.request import Request, urlopen

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


__all__ = [
    "FaceBox",
    "VisionAnalysis",
    "analyze_jpeg",
    "describe_with_vision_api",
    "init_analyzer",
    "is_detector_available",
]
