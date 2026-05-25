"""Async UART transport for development fallback."""

from __future__ import annotations

import asyncio
import logging

from .base import Transport

log = logging.getLogger(__name__)


class UartTransport(Transport):
    """Async serial transport via pyserial-asyncio."""

    def __init__(
        self,
        port: str,
        baudrate: int = 115200,
        connect_timeout: float = 3.0,
    ) -> None:
        self._port = port
        self._baudrate = baudrate
        self._connect_timeout = connect_timeout
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None

    async def connect(self) -> None:
        try:
            import serial_asyncio  # type: ignore[import]
        except ImportError as exc:
            raise ImportError(
                "pyserial-asyncio e necessario para UartTransport. "
                "Instale com: pip install pyserial-asyncio"
            ) from exc

        log.debug("UART: abrindo %s @ %d baud...", self._port, self._baudrate)
        self._reader, self._writer = await asyncio.wait_for(
            serial_asyncio.open_serial_connection(
                url=self._port,
                baudrate=self._baudrate,
            ),
            timeout=self._connect_timeout,
        )
        log.info("UART: conectado a %s", self._port)

    async def disconnect(self) -> None:
        if self._writer is not None:
            try:
                self._writer.close()
                await self._writer.wait_closed()
            except Exception:
                pass
        self._reader = None
        self._writer = None
        log.debug("UART: desconectado de %s", self._port)

    async def send(self, data: bytes) -> None:
        if self._writer is None or self._writer.is_closing():
            raise ConnectionError("UART: nao conectado")
        self._writer.write(data)
        await self._writer.drain()

    async def recv(self, n: int = 4096) -> bytes:
        if self._reader is None:
            return b""
        try:
            return await asyncio.wait_for(self._reader.read(n), timeout=0.1)
        except asyncio.TimeoutError:
            return b""
        except OSError as exc:
            log.debug("UART: recv error: %s", exc)
            return b""

    @property
    def is_connected(self) -> bool:
        return self._writer is not None and not self._writer.is_closing()

    @property
    def description(self) -> str:
        return f"UART {self._port}@{self._baudrate}"
