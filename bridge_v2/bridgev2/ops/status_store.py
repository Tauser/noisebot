"""bridgev2.ops.status_store — Estado de runtime compartilhado entre pipeline e ops API."""
from __future__ import annotations

import re
import time
from collections import deque
from dataclasses import dataclass

_SECRET_PATTERNS = (
    re.compile(r"sk-[A-Za-z0-9_-]{8,}"),
    re.compile(r"AIza[0-9A-Za-z_-]{20,}"),
)


@dataclass
class ErrorEntry:
    ts: float
    kind: str       # "rate_limit_429" | "timeout" | "provider_unavailable" | "stt_failure" | ...
    provider: str
    model: str
    message: str    # curta, SEM segredos
    turn_id: int


class StatusStore:
    """Estado de runtime mutável — o orchestrator escreve, a ops API lê.

    Thread-safety: não é thread-safe. Projetado para rodar no event loop asyncio.
    """

    def __init__(self, max_errors: int = 20) -> None:
        self.firmware_connected: bool = False
        self.last_turn_id: int = 0
        self.last_outcome: str = "ok"   # ok | local_intent | llm | fallback | failed | interrupted
        self.last_transcript: str = ""
        self.last_reply: str = ""
        self.last_route: str = ""
        self._errors: deque[ErrorEntry] = deque(maxlen=max_errors)
        self.turn_counters: dict[str, int] = {
            "total": 0,
            "local_intent": 0,
            "llm": 0,
            "fallback": 0,
            "failed": 0,
            "interrupted": 0,
        }
        # Status por provider (atualizado pelo orchestrator)
        self.stt_status: str = "ok"   # ok | degraded | unavailable
        self.llm_status: str = "ok"
        self.tts_status: str = "ok"

    # -- Writes (chamados pelo Orchestrator) ------------------------------------

    def record_turn_detail(
        self,
        turn_id: int,
        *,
        transcript: str | None = None,
        reply: str | None = None,
        route: str | None = None,
    ) -> None:
        """Atualiza detalhes do último turno sem alterar contadores."""
        self.last_turn_id = turn_id
        if transcript is not None:
            self.last_transcript = _safe_runtime_text(transcript, limit=500)
        if reply is not None:
            self.last_reply = _safe_runtime_text(reply, limit=1200)
        if route is not None:
            self.last_route = _safe_runtime_text(route, limit=40)

    def record_turn(
        self,
        turn_id: int,
        outcome: str,
        *,
        transcript: str | None = None,
        reply: str | None = None,
        route: str | None = None,
    ) -> None:
        self.record_turn_detail(
            turn_id,
            transcript=transcript,
            reply=reply,
            route=route if route is not None else outcome,
        )
        self.last_outcome = outcome
        self.turn_counters["total"] += 1
        if outcome in self.turn_counters:
            self.turn_counters[outcome] += 1

    def record_error(
        self,
        kind: str,
        turn_id: int,
        provider: str = "",
        model: str = "",
        message: str = "",
    ) -> None:
        self._errors.append(ErrorEntry(
            ts=time.time(),
            kind=kind,
            provider=provider,
            model=model,
            message=_safe_runtime_text(message, limit=200),
            turn_id=turn_id,
        ))
        # Degrada status do provider afetado
        if "stt" in kind:
            self.stt_status = "degraded"
        elif any(x in kind for x in ("llm", "rate_limit", "timeout", "provider")):
            self.llm_status = "degraded"
        elif "tts" in kind:
            self.tts_status = "degraded"

    def set_provider_status(self, component: str, status: str) -> None:
        if component == "stt":
            self.stt_status = status
        elif component == "llm":
            self.llm_status = status
        elif component == "tts":
            self.tts_status = status

    # -- Reads (chamados pela ops API) -----------------------------------------

    @property
    def recent_errors(self) -> list[dict]:
        return [
            {
                "ts": e.ts,
                "kind": e.kind,
                "provider": e.provider,
                "model": e.model,
                "message": e.message,
                "turn_id": e.turn_id,
            }
            for e in reversed(self._errors)
        ]

    @property
    def last_error(self) -> dict | None:
        if not self._errors:
            return None
        e = self._errors[-1]
        return {"kind": e.kind, "ts": e.ts, "provider": e.provider}


def _safe_runtime_text(value: str, *, limit: int) -> str:
    text = str(value or "")[:limit]
    for pattern in _SECRET_PATTERNS:
        text = pattern.sub("<redacted>", text)
    return text
