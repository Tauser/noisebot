"""Async firmware transport interface."""

from __future__ import annotations

from abc import ABC, abstractmethod


class Transport(ABC):
    """Async transport interface for TCP and UART links."""

    @abstractmethod
    async def connect(self) -> None:
        """Establish the connection or raise on failure."""

    @abstractmethod
    async def disconnect(self) -> None:
        """Close the connection gracefully."""

    @abstractmethod
    async def send(self, data: bytes) -> None:
        """Send raw bytes."""

    @abstractmethod
    async def recv(self, n: int = 4096) -> bytes:
        """Receive up to n bytes. Return ``b""`` on disconnect."""

    @property
    @abstractmethod
    def is_connected(self) -> bool:
        """True when the transport is active."""

    @property
    @abstractmethod
    def description(self) -> str:
        """Human-readable transport description."""
