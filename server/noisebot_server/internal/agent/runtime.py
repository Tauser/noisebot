"""Conversation runtime primitives owned by the NoiseBot server."""

from __future__ import annotations

import asyncio
import logging
import time
from dataclasses import dataclass, field
from enum import Enum, IntEnum, auto
from typing import Any

log = logging.getLogger(__name__)

_SENTINEL = object()
_TURN_COUNTER = 0


def _now() -> float:
    return time.monotonic()


class EventBus:
    """Async typed event bus with per-subscriber queues."""

    def __init__(self, default_maxsize: int = 128) -> None:
        self._default_maxsize = default_maxsize
        self._subscribers: list[tuple[tuple[type, ...], asyncio.Queue[Any]]] = []
        self._closed = False

    def subscribe(self, *event_types: type, maxsize: int = 0) -> asyncio.Queue[Any]:
        size = maxsize if maxsize != 0 else self._default_maxsize
        if size < 0:
            size = 0
        queue: asyncio.Queue[Any] = asyncio.Queue(maxsize=size)
        self._subscribers.append((event_types, queue))
        return queue

    def unsubscribe(self, queue: asyncio.Queue[Any]) -> None:
        self._subscribers = [
            (types, subscriber)
            for types, subscriber in self._subscribers
            if subscriber is not queue
        ]

    async def publish(self, event: Any) -> None:
        if self._closed:
            return
        for event_types, queue in self._subscribers:
            if event_types and not isinstance(event, event_types):
                continue
            try:
                queue.put_nowait(event)
            except asyncio.QueueFull:
                log.warning(
                    "bus.publish: fila cheia para %s -- evento descartado",
                    type(event).__name__,
                )

    def publish_nowait(self, event: Any) -> None:
        if self._closed:
            return
        for event_types, queue in self._subscribers:
            if event_types and not isinstance(event, event_types):
                continue
            try:
                queue.put_nowait(event)
            except asyncio.QueueFull:
                log.warning(
                    "bus.publish_nowait: fila cheia -- %s descartado",
                    type(event).__name__,
                )

    async def close(self) -> None:
        self._closed = True
        for _, queue in self._subscribers:
            try:
                queue.put_nowait(_SENTINEL)
            except asyncio.QueueFull:
                pass
        self._subscribers.clear()

    @staticmethod
    async def iter_queue(queue: asyncio.Queue[Any]):
        while True:
            event = await queue.get()
            if event is _SENTINEL:
                queue.task_done()
                break
            yield event
            queue.task_done()

    @property
    def subscriber_count(self) -> int:
        return len(self._subscribers)


class VoiceEndReason(IntEnum):
    SILENCE = 0
    TIMEOUT = 1
    BRIDGE_DISCONNECTED = 2
    CANCELLED = 3


class TranscriptQuality(IntEnum):
    GOOD = 0
    LOW_RMS = 1
    NO_SPEECH = 2
    LOW_LOGPROB = 3
    HIGH_COMPRESSION = 4
    EMPTY = 5


@dataclass(frozen=True)
class FirmwareConnected:
    t: float = field(default_factory=_now)
    peer_capabilities: dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class FirmwareDisconnected:
    t: float = field(default_factory=_now)
    reason: str = ""


@dataclass(frozen=True)
class WakeDetected:
    t: float = field(default_factory=_now)
    session_hint: int = 0


@dataclass(frozen=True)
class VoiceActivityStart:
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class AudioChunkIn:
    pcm: bytes
    seq: int = 0
    t_recv: float = field(default_factory=_now)


@dataclass(frozen=True)
class VoiceActivityEnd:
    reason: VoiceEndReason = VoiceEndReason.SILENCE
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class StatusUpdate:
    state: int
    valence: float
    activation: float
    attention: float
    health: int
    volume: int | None = None
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class PartialTranscript:
    turn_id: int
    text: str
    stable: bool = False
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class FinalTranscript:
    turn_id: int
    text: str
    quality: TranscriptQuality = TranscriptQuality.GOOD
    no_speech_prob: float = 0.0
    avg_logprob: float = 0.0
    compression_ratio: float = 1.0
    t: float = field(default_factory=_now)

    @property
    def is_usable(self) -> bool:
        return self.quality == TranscriptQuality.GOOD and bool(self.text.strip())


@dataclass(frozen=True)
class TurnCommitted:
    turn_id: int
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class IntentResolved:
    turn_id: int
    intent_name: str | None
    reply_text: str | None = None
    expression_id: int | None = None
    action_id: int | None = None
    emot_event_id: int | None = None
    device_command: dict[str, Any] | None = None
    resolution_reason: str | None = None
    t: float = field(default_factory=_now)

    @property
    def has_intent(self) -> bool:
        return self.intent_name is not None


@dataclass(frozen=True)
class LlmTokenDelta:
    turn_id: int
    text: str
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class LlmReplyComplete:
    turn_id: int
    reply: str
    expression_id: int | None = None
    action_id: int | None = None
    emot_event_id: int | None = None
    input_tokens: int = 0
    output_tokens: int = 0
    provider: str = ""
    model: str = ""
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class SentenceReady:
    turn_id: int
    sentence: str
    index: int = 0
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class TtsAudioChunk:
    turn_id: int
    pcm: bytes
    sentence_index: int = 0
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class TtsSentenceDone:
    turn_id: int
    sentence_index: int = 0
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class SpeechDone:
    turn_id: int
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class RobotCommand:
    kind: str
    payload: dict[str, Any] = field(default_factory=dict)
    turn_id: int = 0
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class SayChunkOut:
    turn_id: int
    pcm: bytes
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class SpeechCancel:
    turn_id: int
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class BargeInDetected:
    turn_id: int
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class TurnError:
    turn_id: int
    stage: str
    reason: str
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class ShutdownRequested:
    reason: str = "shutdown"
    t: float = field(default_factory=_now)


def new_turn_id() -> int:
    global _TURN_COUNTER
    _TURN_COUNTER += 1
    return _TURN_COUNTER


@dataclass
class SessionContext:
    turn_id: int = field(default_factory=new_turn_id)
    t_start: float = field(default_factory=_now)
    audio_chunks: list[bytes] = field(default_factory=list)
    total_samples: int = 0
    partial_text: str = ""
    final_text: str = ""
    reply_text: str = ""
    intent_name: str | None = None
    llm_provider: str = ""
    llm_model: str = ""
    input_tokens: int = 0
    output_tokens: int = 0
    timeline: dict[str, float | None] = field(default_factory=dict)
    deadline: float | None = None
    discard_reason: str | None = None
    meta: dict[str, Any] = field(default_factory=dict)

    def append_audio(self, pcm: bytes) -> None:
        self.audio_chunks.append(pcm)
        self.total_samples += len(pcm) // 2

    def full_pcm(self) -> bytes:
        return b"".join(self.audio_chunks)

    def duration_s(self) -> float:
        return self.total_samples / 16000

    def mark(self, name: str, t: float | None = None) -> None:
        self.timeline[name] = t if t is not None else _now()

    def elapsed_since_start(self) -> float:
        return _now() - self.t_start

    def is_past_deadline(self) -> bool:
        if self.deadline is None:
            return False
        return _now() > self.deadline

    def set_deadline(self, seconds: float) -> None:
        self.deadline = _now() + seconds

    def to_log_dict(self) -> dict[str, Any]:
        return {
            "turn_id": self.turn_id,
            "total_samples": self.total_samples,
            "duration_s": round(self.duration_s(), 2),
            "partial_text": self.partial_text[:80],
            "final_text": self.final_text[:80],
            "intent_name": self.intent_name,
            "llm_provider": self.llm_provider,
            "llm_model": self.llm_model,
            "input_tokens": self.input_tokens,
            "output_tokens": self.output_tokens,
            "discard_reason": self.discard_reason,
            "timeline": {
                key: round(value, 4)
                for key, value in self.timeline.items()
                if value is not None
            },
        }


class TurnState(Enum):
    IDLE = auto()
    LISTENING = auto()
    COMMITTING_TURN = auto()
    THINKING = auto()
    SPEAKING = auto()
    INTERRUPTED = auto()
    ERROR_RECOVERY = auto()


_VALID_TRANSITIONS: dict[TurnState, frozenset[TurnState]] = {
    TurnState.IDLE: frozenset({TurnState.LISTENING}),
    TurnState.LISTENING: frozenset({
        TurnState.LISTENING,
        TurnState.COMMITTING_TURN,
        TurnState.IDLE,
        TurnState.INTERRUPTED,
        TurnState.ERROR_RECOVERY,
    }),
    TurnState.COMMITTING_TURN: frozenset({
        TurnState.THINKING,
        TurnState.IDLE,
        TurnState.ERROR_RECOVERY,
    }),
    TurnState.THINKING: frozenset({
        TurnState.SPEAKING,
        TurnState.IDLE,
        TurnState.INTERRUPTED,
        TurnState.ERROR_RECOVERY,
    }),
    TurnState.SPEAKING: frozenset({
        TurnState.SPEAKING,
        TurnState.IDLE,
        TurnState.INTERRUPTED,
        TurnState.ERROR_RECOVERY,
    }),
    TurnState.INTERRUPTED: frozenset({
        TurnState.LISTENING,
        TurnState.IDLE,
        TurnState.ERROR_RECOVERY,
    }),
    TurnState.ERROR_RECOVERY: frozenset({TurnState.IDLE}),
}


class TurnManager:
    """Turn-taking state machine."""

    def __init__(self) -> None:
        self._state = TurnState.IDLE
        self._current_turn_id = 0

    @property
    def state(self) -> TurnState:
        return self._state

    @property
    def current_turn_id(self) -> int:
        return self._current_turn_id

    @property
    def is_idle(self) -> bool:
        return self._state == TurnState.IDLE

    @property
    def is_listening(self) -> bool:
        return self._state == TurnState.LISTENING

    @property
    def can_speak(self) -> bool:
        return self._state == TurnState.SPEAKING

    @property
    def can_interrupt(self) -> bool:
        return self._state in (TurnState.THINKING, TurnState.SPEAKING)

    def transition(self, new_state: TurnState, turn_id: int | None = None) -> None:
        allowed = _VALID_TRANSITIONS.get(self._state, frozenset())
        if new_state not in allowed:
            raise ValueError(f"Transicao invalida: {self._state.name} -> {new_state.name}")

        old_state = self._state
        if turn_id is not None:
            self._current_turn_id = turn_id

        exit_cb = getattr(self, f"on_exit_{old_state.name.lower()}", None)
        if callable(exit_cb):
            exit_cb()

        self._state = new_state
        log.debug(
            "FSM: %s -> %s (turn_id=%d)",
            old_state.name,
            new_state.name,
            self._current_turn_id,
        )

        enter_cb = getattr(self, f"on_enter_{new_state.name.lower()}", None)
        if callable(enter_cb):
            enter_cb()

    def try_transition(self, new_state: TurnState, turn_id: int | None = None) -> bool:
        try:
            self.transition(new_state, turn_id)
            return True
        except ValueError:
            log.warning(
                "FSM: transicao ignorada %s -> %s",
                self._state.name,
                new_state.name,
            )
            return False

    def reset_to_idle(self) -> None:
        old = self._state
        self._state = TurnState.IDLE
        log.debug("FSM: reset forcado %s -> IDLE", old.name)


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
    "PartialTranscript",
    "RobotCommand",
    "SayChunkOut",
    "SentenceReady",
    "SessionContext",
    "ShutdownRequested",
    "SpeechCancel",
    "SpeechDone",
    "StatusUpdate",
    "TranscriptQuality",
    "TtsAudioChunk",
    "TtsSentenceDone",
    "TurnCommitted",
    "TurnError",
    "TurnManager",
    "TurnState",
    "VoiceActivityEnd",
    "VoiceActivityStart",
    "VoiceEndReason",
    "WakeDetected",
    "new_turn_id",
]
