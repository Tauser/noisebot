"""STT provider facade."""

from __future__ import annotations

from ..._compat import ensure_bridgev2_path

ensure_bridgev2_path()

from bridgev2.stt.base import STTProvider
from bridgev2.stt.whisper_local import WhisperLocalSTT

__all__ = ["STTProvider", "WhisperLocalSTT"]
