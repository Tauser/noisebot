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
_LOW_LIGHT_LUMA_THRESHOLD = 75
_LOW_CONTRAST_THRESHOLD = 70

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


def _env_float(key: str, default: float) -> float:
    try:
        return float(os.environ.get(key, default))
    except (TypeError, ValueError):
        return default


def _detect_faces(image: Any) -> list["FaceBox"]:
    h, w = image.shape[:2]
    _detector.setInputSize((w, h))
    _, faces = _detector.detect(image)

    boxes: list[FaceBox] = []
    if faces is not None:
        for face in faces:
            fx, fy, fw, fh = int(face[0]), int(face[1]), int(face[2]), int(face[3])
            if fw > 0 and fh > 0:
                boxes.append(FaceBox(fx, fy, fw, fh))
    return boxes


def _should_try_low_light_enhancement(observation: "VisionObservation") -> bool:
    return (
        observation.luma_avg < _LOW_LIGHT_LUMA_THRESHOLD
        or observation.contrast < _LOW_CONTRAST_THRESHOLD
    )


def _enhance_low_light_image(image: Any, cv2: Any) -> Any:
    """Realça uma cópia do frame para ajudar YuNet em baixa luz."""
    lab = cv2.cvtColor(image, cv2.COLOR_BGR2LAB)
    l_chan, a_chan, b_chan = cv2.split(lab)
    clahe = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))
    l_chan = clahe.apply(l_chan)
    enhanced = cv2.merge((l_chan, a_chan, b_chan))
    enhanced = cv2.cvtColor(enhanced, cv2.COLOR_LAB2BGR)
    return cv2.convertScaleAbs(enhanced, alpha=1.25, beta=12)


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

    boxes = _detect_faces(image)
    if not boxes and _should_try_low_light_enhancement(observation):
        enhanced = _enhance_low_light_image(image, cv2)
        boxes = _detect_faces(enhanced)

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
    "Voce e os olhos do NoiseBot, um robo companion de mesa. Responda em portugues "
    "do Brasil, com 2 a 6 frases naturais para fala em voz alta. Descreva o que "
    "voce ve como se estivesse conversando com a pessoa na sua frente. Se houver "
    "uma pessoa ou rosto, trate como \"voce\" e descreva sua posicao, postura ou "
    "expressao aparente, sem inventar identidade. Mencione ambiente, luz e objetos "
    "relevantes quando forem visiveis. Evite termos tecnicos como resolucao, pixels, "
    "detector, bounding box, modelo ou camera. Se algo estiver incerto, use \"parece\" "
    "em vez de afirmar com certeza."
)

_VISION_API_TIMEOUT_S = 8.0
_OLLAMA_VISION_TIMEOUT_S = 25.0
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


def describe_with_ollama_vision(
    jpeg_bytes: bytes,
    *,
    base_url: str | None = None,
    model: str | None = None,
) -> str | None:
    """Call a local Ollama multimodal model to describe the JPEG content."""
    from urllib.error import HTTPError, URLError
    from urllib.request import Request, urlopen

    if not jpeg_bytes or len(jpeg_bytes) > _VISION_MAX_JPEG_BYTES:
        return None

    selected_model = (model or os.environ.get("NOISEBOT_LLM_MODEL", "gemma4:12b")).strip()
    if not selected_model or selected_model == "none":
        return None

    endpoint_base = (base_url or os.environ.get(
        "NOISEBOT_OLLAMA_BASE_URL",
        "http://127.0.0.1:11434",
    )).rstrip("/")
    timeout_s = _env_float("NOISEBOT_VISION_LLM_TIMEOUT_S", _OLLAMA_VISION_TIMEOUT_S)
    encoded_image = base64.b64encode(jpeg_bytes).decode("ascii")

    chat_body = {
        "model": selected_model,
        "stream": False,
        "think": False,
        "messages": [
            {
                "role": "user",
                "content": _VISION_DESCRIBE_PROMPT,
                "images": [encoded_image],
            }
        ],
        "options": {
            "temperature": 0.2,
            "num_predict": 150,
        },
    }

    chat_data = _post_ollama_json(
        endpoint_base + "/api/chat",
        chat_body,
        timeout_s=timeout_s,
        urlopen_fn=urlopen,
    )
    if chat_data is not None:
        content = _ollama_content_from_payload(chat_data)
        if content:
            return content
        log.warning(
            "Ollama vision /api/chat sem conteudo. model=%s done_reason=%s "
            "eval_count=%s message_keys=%s keys=%s",
            selected_model,
            chat_data.get("done_reason"),
            chat_data.get("eval_count"),
            sorted(chat_data.get("message", {}).keys())
            if isinstance(chat_data.get("message"), dict) else [],
            sorted(chat_data.keys()),
        )

    generate_body = {
        "model": selected_model,
        "prompt": _VISION_DESCRIBE_PROMPT,
        "images": [encoded_image],
        "stream": False,
        "think": False,
        "options": {
            "temperature": 0.2,
            "num_predict": 150,
        },
    }
    generate_data = _post_ollama_json(
        endpoint_base + "/api/generate",
        generate_body,
        timeout_s=timeout_s,
        urlopen_fn=urlopen,
    )
    if generate_data is not None:
        content = _ollama_content_from_payload(generate_data)
        if content:
            return content
        log.warning(
            "Ollama vision /api/generate sem conteudo. model=%s done_reason=%s "
            "eval_count=%s keys=%s",
            selected_model,
            generate_data.get("done_reason"),
            generate_data.get("eval_count"),
            sorted(generate_data.keys()),
        )
    return None


def _post_ollama_json(
    url: str,
    body: dict[str, Any],
    *,
    timeout_s: float,
    urlopen_fn,
) -> dict[str, Any] | None:
    from urllib.error import HTTPError, URLError
    from urllib.request import Request

    req = Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        method="POST",
        headers={
            "Content-Type": "application/json",
            "User-Agent": "NoiseBot-Server/0.1",
        },
    )
    try:
        with urlopen_fn(req, timeout=timeout_s) as resp:
            data = json.loads(resp.read().decode("utf-8"))
        return data if isinstance(data, dict) else None
    except HTTPError as exc:
        try:
            detail = exc.read().decode("utf-8")[:240]
        except Exception:
            detail = str(exc)
        log.warning("Ollama vision HTTP %s em %s: %s", exc.code, url, detail)
        return None
    except (URLError, TimeoutError, OSError) as exc:
        log.warning("Ollama vision falhou em %s: %s", url, exc)
        return None
    except json.JSONDecodeError as exc:
        log.warning("Ollama vision JSON invalido em %s: %s", url, exc)
        return None


def _ollama_content_from_payload(data: dict[str, Any]) -> str | None:
    message = data.get("message")
    if isinstance(message, dict):
        content = message.get("content")
        if isinstance(content, str) and content.strip():
            return content.strip()
    response = data.get("response")
    if isinstance(response, str) and response.strip():
        return response.strip()
    return None


__all__ = [
    "FaceBox",
    "VisionAnalysis",
    "analyze_jpeg",
    "describe_with_ollama_vision",
    "describe_with_vision_api",
    "init_analyzer",
    "is_detector_available",
]
