"""bridgev2.stt.base — Interface STTProvider."""
from __future__ import annotations
from abc import ABC, abstractmethod
from ..runtime.events import PartialTranscript, FinalTranscript


class STTProvider(ABC):
    @abstractmethod
    async def initialize(self) -> None:
        """Carrega modelo / inicia processo. Pode bloquear — chamar uma vez no boot."""

    @abstractmethod
    def feed(self, pcm: bytes) -> None:
        """Alimenta chunk PCM para transcrição parcial (best-effort)."""

    @abstractmethod
    async def partial(self, turn_id: int) -> PartialTranscript:
        """Melhor estimativa atual (pode ser vazia se não houver dados)."""

    @abstractmethod
    async def finalize(self, full_pcm: bytes, turn_id: int) -> FinalTranscript:
        """Transcrição final sobre o buffer completo do turno."""

    @abstractmethod
    async def reset(self) -> None:
        """Limpa estado parcial (novo turno)."""
