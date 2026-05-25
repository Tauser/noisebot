"""Conversation runtime primitives facade."""

from __future__ import annotations

from ..._compat import ensure_bridgev2_path

ensure_bridgev2_path()

from bridgev2.runtime.bus import EventBus
from bridgev2.runtime.events import (
    AudioChunkIn,
    BargeInDetected,
    FinalTranscript,
    FirmwareConnected,
    FirmwareDisconnected,
    IntentResolved,
    LlmReplyComplete,
    LlmTokenDelta,
    RobotCommand,
    SentenceReady,
    ShutdownRequested,
    SpeechCancel,
    SpeechDone,
    StatusUpdate,
    TranscriptQuality,
    TurnError,
    VoiceActivityEnd,
    VoiceActivityStart,
    VoiceEndReason,
    WakeDetected,
)
from bridgev2.runtime.session import SessionContext, new_turn_id
from bridgev2.runtime.turn_manager import TurnManager, TurnState

__all__ = [
    "AudioChunkIn",
    "BargeInDetected",
    "EventBus",
    "FinalTranscript",
    "FirmwareConnected",
    "FirmwareDisconnected",
    "IntentResolved",
    "LlmReplyComplete",
    "LlmTokenDelta",
    "RobotCommand",
    "SentenceReady",
    "SessionContext",
    "ShutdownRequested",
    "SpeechCancel",
    "SpeechDone",
    "StatusUpdate",
    "TranscriptQuality",
    "TurnError",
    "TurnManager",
    "TurnState",
    "VoiceActivityEnd",
    "VoiceActivityStart",
    "VoiceEndReason",
    "WakeDetected",
    "new_turn_id",
]
