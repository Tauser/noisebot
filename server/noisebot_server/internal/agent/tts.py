"""TTS provider facade."""

from __future__ import annotations

from ..._compat import ensure_bridgev2_path

ensure_bridgev2_path()

from bridgev2.tts.base import TTSProvider
from bridgev2.tts.cache import PhrasePcmCache
from bridgev2.tts.piper_server import PiperServerTTS
from bridgev2.tts.sentencizer import Sentencizer

__all__ = [
    "PhrasePcmCache",
    "PiperServerTTS",
    "Sentencizer",
    "TTSProvider",
]
