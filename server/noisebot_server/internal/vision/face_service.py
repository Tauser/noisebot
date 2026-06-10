"""Face enrollment and identification via local Ollama vision model.

Flow:
  enroll(user_id, display_name, jpeg) → save reference photo
  identify(jpeg) → Haar Cascade confirms face → Ollama compares → user_id | None
"""
from __future__ import annotations

import base64
import json
import logging
import os
from dataclasses import dataclass
from urllib.error import URLError
from urllib.request import Request, urlopen

from .analysis import FaceBox, analyze_jpeg
from .client import VisionObservation
from .face_store import FaceStore

log = logging.getLogger(__name__)

_ENROLL_MIN_BYTES = 1_000
_IDENTIFY_TIMEOUT_S = 10.0
_IDENTIFY_MAX_JPEG = 800_000

_IDENTIFY_PROMPT = (
    "Você receberá duas imagens: a primeira é a foto de referência cadastrada de uma pessoa "
    "chamada '{name}' (user_id: {uid}). A segunda é a imagem ao vivo da câmera.\n"
    "Responda APENAS com um JSON: {{\"match\": true}} se for a mesma pessoa, "
    "ou {{\"match\": false}} se não for ou se não houver rosto visível."
)

_UNKNOWN_PROMPT = (
    "Existe um rosto humano claramente visível e focado nesta imagem? "
    "Responda APENAS com JSON: {\"face\": true} ou {\"face\": false}."
)


@dataclass
class IdentifyResult:
    user_id: str | None
    display_name: str | None
    confidence: str  # "high" | "low" | "unknown"
    face_box: FaceBox | None


class FaceService:
    def __init__(
        self,
        store: FaceStore | None = None,
        ollama_base_url: str = "http://127.0.0.1:11434",
        ollama_model: str = "gemma4:12b",
    ) -> None:
        self._store = store or FaceStore()
        self._ollama_base = ollama_base_url.rstrip("/")
        self._model = ollama_model

    # ── Enrollment ─────────────────────────────────────────────────────────

    def enroll(self, user_id: str, display_name: str, jpeg_bytes: bytes) -> None:
        if len(jpeg_bytes) < _ENROLL_MIN_BYTES:
            raise ValueError("JPEG muito pequeno para enrollment")
        # Confirm there's actually a face in the photo
        dummy_obs = VisionObservation(
            valid=True, scene="normal", timestamp_ms=0,
            width=0, height=0, jpeg_bytes=len(jpeg_bytes),
            capture_ms=0, luma_avg=128, luma_min=0, luma_max=255,
            contrast=50, motion_score=0,
        )
        analysis = analyze_jpeg(jpeg_bytes, dummy_obs)
        if not analysis.face_detected:
            raise ValueError("Nenhum rosto detectado na foto de enrollment")
        self._store.save(user_id, display_name, jpeg_bytes)
        log.info("Face enrolled: user_id=%s name=%s", user_id, display_name)

    # ── Identification ──────────────────────────────────────────────────────

    def identify(self, jpeg_bytes: bytes) -> IdentifyResult:
        """Identify who is in the frame.

        Returns IdentifyResult with user_id=None if unknown or no face found.
        """
        if not jpeg_bytes or len(jpeg_bytes) > _IDENTIFY_MAX_JPEG:
            return IdentifyResult(None, None, "unknown", None)

        # Step 1: Haar Cascade — fast check, no LLM cost if no face
        dummy_obs = VisionObservation(
            valid=True, scene="normal", timestamp_ms=0,
            width=0, height=0, jpeg_bytes=len(jpeg_bytes),
            capture_ms=0, luma_avg=128, luma_min=0, luma_max=255,
            contrast=50, motion_score=0,
        )
        analysis = analyze_jpeg(jpeg_bytes, dummy_obs)
        if not analysis.face_detected:
            return IdentifyResult(None, None, "unknown", None)

        face_box = analysis.primary_face

        # Step 2: Compare against each enrolled reference
        references = self._store.load_all()
        if not references:
            return IdentifyResult(None, None, "unknown", face_box)

        meta = {u["user_id"]: u["display_name"] for u in self._store.list_users()}

        for uid, ref_jpeg in references.items():
            display_name = meta.get(uid, uid)
            prompt = _IDENTIFY_PROMPT.format(name=display_name, uid=uid)
            matched = _ollama_compare(
                prompt=prompt,
                image_a=ref_jpeg,
                image_b=jpeg_bytes,
                base_url=self._ollama_base,
                model=self._model,
            )
            if matched:
                log.info("Face identified: user_id=%s", uid)
                return IdentifyResult(uid, display_name, "high", face_box)

        return IdentifyResult(None, None, "low", face_box)

    def update_model(self, model: str) -> None:
        self._model = model

    def update_base_url(self, base_url: str) -> None:
        self._ollama_base = base_url.rstrip("/")


# ── Ollama vision helpers ───────────────────────────────────────────────────

def _ollama_compare(
    prompt: str,
    image_a: bytes,
    image_b: bytes,
    base_url: str,
    model: str,
) -> bool:
    """Send two images to Ollama and ask if they match. Returns True on match."""
    b64_a = base64.b64encode(image_a).decode("ascii")
    b64_b = base64.b64encode(image_b).decode("ascii")
    body = {
        "model": model,
        "messages": [{
            "role": "user",
            "content": prompt,
            "images": [b64_a, b64_b],
        }],
        "stream": False,
        "options": {"temperature": 0.0, "num_predict": 32},
    }
    raw = _ollama_post(base_url, body)
    if raw is None:
        return False
    return _parse_match(raw)


def _ollama_post(base_url: str, body: dict) -> str | None:
    payload = json.dumps(body).encode("utf-8")
    req = Request(
        f"{base_url}/api/chat",
        data=payload,
        method="POST",
        headers={"Content-Type": "application/json"},
    )
    try:
        with urlopen(req, timeout=_IDENTIFY_TIMEOUT_S) as resp:
            data = json.loads(resp.read().decode("utf-8"))
        return str(data["message"]["content"]).strip()
    except (URLError, OSError, KeyError, json.JSONDecodeError) as exc:
        log.warning("Ollama vision call failed: %s", exc)
        return None


def _parse_match(raw: str) -> bool:
    import re
    cleaned = raw.strip()
    # Try to find JSON object
    m = re.search(r"\{[^}]*\}", cleaned)
    if m:
        try:
            obj = json.loads(m.group(0))
            return bool(obj.get("match", False))
        except (ValueError, TypeError):
            pass
    # Fallback: look for true/false keywords
    lower = cleaned.lower()
    if '"match": true' in lower or '"match":true' in lower:
        return True
    return False


__all__ = ["FaceService", "IdentifyResult"]
