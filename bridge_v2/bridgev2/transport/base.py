"""bridgev2.transport.base — Interface async de transporte."""
from __future__ import annotations

from abc import ABC, abstractmethod


class Transport(ABC):
    """Interface de transporte async (TCP ou UART).

    Implementações: tcp.TcpTransport, uart.UartTransport (Fase 2).
    """

    @abstractmethod
    async def connect(self) -> None:
        """Estabelece conexão. Lança exceção em caso de falha."""

    @abstractmethod
    async def disconnect(self) -> None:
        """Fecha a conexão graciosamente."""

    @abstractmethod
    async def send(self, data: bytes) -> None:
        """Envia bytes brutos. Lança exceção se desconectado."""

    @abstractmethod
    async def recv(self, n: int = 4096) -> bytes:
        """Recebe até n bytes. Retorna b'' em caso de desconexão."""

    @property
    @abstractmethod
    def is_connected(self) -> bool:
        """True se a conexão está ativa."""

    @property
    @abstractmethod
    def description(self) -> str:
        """Descrição legível da conexão (ex.: 'TCP 192.168.1.10:9000')."""
