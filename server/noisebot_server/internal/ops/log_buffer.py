"""Buffer circular de logs para a Ops API.

Mantem uma janela curta em memoria para o dashboard Dev sem depender de arquivo
ou de stream externo. O buffer evita expor segredos comuns antes de serializar.
"""
from __future__ import annotations

from collections import deque
from dataclasses import dataclass
import logging
import re
import time
from typing import Deque

_SECRET_PATTERNS = (
    re.compile(r"sk-[A-Za-z0-9_\-]{16,}"),
    re.compile(r"(Bearer\s+)[A-Za-z0-9_\-\.]+", re.IGNORECASE),
    re.compile(r"((?:api[_-]?key|token|secret)\s*[=:]\s*)[^\s,;]+", re.IGNORECASE),
)


@dataclass(frozen=True)
class LogEntry:
    ts: float
    level: str
    logger: str
    message: str

    def to_dict(self) -> dict:
        return {
            "ts": self.ts,
            "level": self.level,
            "logger": self.logger,
            "message": self.message,
        }


class RecentLogBuffer:
    def __init__(self, max_entries: int = 300) -> None:
        self._entries: Deque[LogEntry] = deque(maxlen=max_entries)

    def append_record(self, record: logging.LogRecord) -> None:
        message = _redact(record.getMessage())
        self._entries.append(
            LogEntry(
                ts=record.created or time.time(),
                level=record.levelname,
                logger=record.name,
                message=message,
            )
        )

    def recent(self, limit: int = 80) -> list[dict]:
        safe_limit = max(1, min(limit, 300))
        entries = list(self._entries)[-safe_limit:]
        return [entry.to_dict() for entry in reversed(entries)]

    @property
    def count(self) -> int:
        return len(self._entries)


class RecentLogHandler(logging.Handler):
    def __init__(self, buffer: RecentLogBuffer) -> None:
        super().__init__(level=logging.INFO)
        self._buffer = buffer

    def emit(self, record: logging.LogRecord) -> None:
        try:
            self._buffer.append_record(record)
        except Exception:
            self.handleError(record)


RECENT_LOGS = RecentLogBuffer()
_handler: RecentLogHandler | None = None


def install_recent_log_handler() -> RecentLogBuffer:
    global _handler
    if _handler is None:
        _handler = RecentLogHandler(RECENT_LOGS)
        logging.getLogger().addHandler(_handler)
    return RECENT_LOGS


def _redact(value: str) -> str:
    redacted = value
    for pattern in _SECRET_PATTERNS:
        redacted = pattern.sub(_replace_secret, redacted)
    return redacted


def _replace_secret(match: re.Match[str]) -> str:
    if match.lastindex:
        return f"{match.group(1)}<redacted>"
    return "<redacted>"
