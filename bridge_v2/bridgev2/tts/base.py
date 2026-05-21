"""bridgev2.tts.base — Interface TTSProvider."""
from __future__ import annotations
from abc import ABC, abstractmethod
from typing import AsyncIterator


class TTSProvider(ABC):
    @abstractmethod
    async def initialize(self) -> None:
        """Inicia processo/servidor TTS. Pode bloquear — chamar uma vez no boot."""

    @abstractmethod
    async def synthesize_stream(self, sentences: AsyncIterator[str]) -> AsyncIterator[bytes]:
        """Sintetiza frases em streaming, retornando chunks PCM int16 16 kHz."""

    @abstractmethod
    async def shutdown(self) -> None:
        """Encerra o processo/servidor TTS."""
