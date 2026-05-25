"""STT, LLM, TTS and turn orchestration internals.

Phase 4 owns the server-side Agent boundary while preserving the existing
``bridge_v2`` implementation. New code should import conversation runtime,
local intents, LLM providers, STT and TTS through this package.
"""

from __future__ import annotations

from .intents import LocalIntentProvider
from .llm import LLMProvider, StreamingLLMProvider
from .orchestrator import Orchestrator
from .runtime import EventBus, SessionContext, TurnManager, TurnState
from .stt import STTProvider, WhisperLocalSTT
from .tts import PiperServerTTS, Sentencizer, TTSProvider

__all__ = [
    "EventBus",
    "LLMProvider",
    "LocalIntentProvider",
    "Orchestrator",
    "PiperServerTTS",
    "STTProvider",
    "Sentencizer",
    "SessionContext",
    "StreamingLLMProvider",
    "TTSProvider",
    "TurnManager",
    "TurnState",
    "WhisperLocalSTT",
]
