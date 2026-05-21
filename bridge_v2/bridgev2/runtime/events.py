"""bridgev2.runtime.events — Dataclasses de eventos internos do bus.

Todos os eventos são imutáveis (frozen=True). O bus transporta apenas estes tipos.
Nenhum evento contém segredos.

Organização:
  - Eventos de firmware → bridge   (prefixo Fw*)
  - Eventos de processamento        (internos)
  - Eventos de saída bridge → fw   (prefixo Robot*)
  - Eventos de controle            (Turn*, Barge*)
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field
from enum import IntEnum
from typing import Any


def _now() -> float:
    return time.monotonic()


# ── Enums de razão ─────────────────────────────────────────────────────────


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


# ── Eventos firmware → bridge ──────────────────────────────────────────────


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
    pcm: bytes          # int16 LE, 256 amostras, 16 kHz mono
    seq: int = 0        # número de sequência (0 = sem seq)
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
    t: float = field(default_factory=_now)


# ── Eventos de processamento ───────────────────────────────────────────────


@dataclass(frozen=True)
class PartialTranscript:
    turn_id: int
    text: str
    stable: bool = False   # True = segmento estável, não vai mudar
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
    intent_name: str | None   # None = sem intent local → vai à LLM
    reply_text: str | None = None
    expression_id: int | None = None   # hint para RobotOutputProvider
    action_id: int | None = None       # hint para RobotOutputProvider
    emot_event_id: int | None = None   # hint para RobotOutputProvider
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
    """Uma frase de texto pronta para ser enviada ao TTS."""
    turn_id: int
    sentence: str
    index: int = 0       # 0 = primeira frase
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class TtsAudioChunk:
    turn_id: int
    pcm: bytes            # int16 LE, 16 kHz mono
    sentence_index: int = 0
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class TtsSentenceDone:
    turn_id: int
    sentence_index: int = 0
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class SpeechDone:
    """Output Scheduler terminou de enviar todo o áudio do turno."""
    turn_id: int
    t: float = field(default_factory=_now)


# ── Eventos de saída bridge → firmware ────────────────────────────────────


@dataclass(frozen=True)
class RobotCommand:
    """Comando de alto nível para o firmware (expr/action/gaze/emot/text/volume)."""
    kind: str          # "expr" | "action" | "gaze" | "emot" | "text" | "volume"
    payload: dict[str, Any] = field(default_factory=dict)
    turn_id: int = 0
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class SayChunkOut:
    turn_id: int
    pcm: bytes   # int16 LE, ≤256 amostras, 16 kHz mono
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class SpeechCancel:
    turn_id: int
    t: float = field(default_factory=_now)


# ── Eventos de controle ────────────────────────────────────────────────────


@dataclass(frozen=True)
class BargeInDetected:
    turn_id: int
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class TurnError:
    turn_id: int
    stage: str      # "stt" | "llm" | "tts" | "transport" | ...
    reason: str
    t: float = field(default_factory=_now)


@dataclass(frozen=True)
class ShutdownRequested:
    reason: str = "shutdown"
    t: float = field(default_factory=_now)
