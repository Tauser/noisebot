"""bridgev2.llm.base — Interfaces LLMProvider e StreamingLLMProvider."""
from __future__ import annotations
from abc import ABC, abstractmethod
from typing import AsyncIterator
from ..runtime.events import LlmReplyComplete


class LLMProvider(ABC):
    """Provider batch — retorna resposta completa."""
    @abstractmethod
    async def generate(self, text: str, context: dict) -> LlmReplyComplete:
        ...


class StreamingLLMProvider(ABC):
    """Provider com streaming de tokens — preferencial."""
    @abstractmethod
    def generate_stream(self, text: str, context: dict) -> AsyncIterator[str]:
        ...
