"""bridgev2.runtime.session — SessionContext: estado de um turno de conversa.

Cada turno tem um turn_id monotônico único. O SessionContext guarda:
- buffers de áudio PCM do turno
- transcript parcial e final
- linha de tempo de latência (preenchida pelo metrics/timeline)
- deadline de watchdog
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Any


_TURN_COUNTER = 0  # monotônico global; incrementado por new_turn_id()


def new_turn_id() -> int:
    global _TURN_COUNTER
    _TURN_COUNTER += 1
    return _TURN_COUNTER


def _now() -> float:
    return time.monotonic()


@dataclass
class SessionContext:
    """Estado mutável de um turno de conversa.

    Criado ao entrar em LISTENING; descartado ao voltar a IDLE.
    Não é thread-safe — pertence exclusivamente ao event loop.
    """

    turn_id: int = field(default_factory=new_turn_id)
    t_start: float = field(default_factory=_now)

    # ── Buffers de áudio ───────────────────────────────────────────────────
    # Lista de bytes int16 LE (cada elemento = 1 chunk de 256 amostras)
    audio_chunks: list[bytes] = field(default_factory=list)
    total_samples: int = 0

    # ── Transcrições ───────────────────────────────────────────────────────
    partial_text: str = ""
    final_text: str = ""

    # ── Resposta gerada ────────────────────────────────────────────────────
    reply_text: str = ""
    intent_name: str | None = None

    # ── Contexto de LLM ───────────────────────────────────────────────────
    llm_provider: str = ""
    llm_model: str = ""
    input_tokens: int = 0
    output_tokens: int = 0

    # ── Timeline de latência (marcos monotônicos) ──────────────────────────
    # Cada marco é (nome, t_monotonic | None)
    timeline: dict[str, float | None] = field(default_factory=dict)

    # ── Deadline (watchdog) ────────────────────────────────────────────────
    deadline: float | None = None   # tempo monotônico máximo do turno

    # ── Resultado final ────────────────────────────────────────────────────
    discard_reason: str | None = None   # None = turno concluído normalmente

    # ── Metadados extras (uso livre pelos módulos) ─────────────────────────
    meta: dict[str, Any] = field(default_factory=dict)

    # ── Helpers ────────────────────────────────────────────────────────────

    def append_audio(self, pcm: bytes) -> None:
        self.audio_chunks.append(pcm)
        # int16 → 2 bytes por amostra
        self.total_samples += len(pcm) // 2

    def full_pcm(self) -> bytes:
        """Retorna o áudio completo do turno concatenado."""
        return b"".join(self.audio_chunks)

    def duration_s(self) -> float:
        """Duração total de áudio acumulado em segundos."""
        return self.total_samples / 16000

    def mark(self, name: str, t: float | None = None) -> None:
        """Registra um marco de latência."""
        self.timeline[name] = t if t is not None else _now()

    def elapsed_since_start(self) -> float:
        return _now() - self.t_start

    def is_past_deadline(self) -> bool:
        if self.deadline is None:
            return False
        return _now() > self.deadline

    def set_deadline(self, seconds: float) -> None:
        self.deadline = _now() + seconds

    def to_log_dict(self) -> dict:
        """Retorna dict resumido para log estruturado."""
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
            "timeline": {k: round(v, 4) for k, v in self.timeline.items() if v is not None},
        }
