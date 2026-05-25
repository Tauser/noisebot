"""Async TCP client transport for the ESP32 firmware server."""

from __future__ import annotations

import asyncio
import logging

from .base import Transport

log = logging.getLogger(__name__)


class TcpTransport(Transport):
    """Asyncio TCP client with timeout-aware reads."""

    def __init__(
        self,
        host: str,
        port: int = 9000,
        connect_timeout: float = 5.0,
        read_timeout: float = 30.0,
    ) -> None:
        self._host = host
        self._port = port
        self._connect_timeout = connect_timeout
        self._read_timeout = read_timeout
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None

    async def connect(self) -> None:
        log.debug("TCP: conectando %s:%d...", self._host, self._port)
        self._reader, self._writer = await asyncio.wait_for(
            asyncio.open_connection(self._host, self._port),
            timeout=self._connect_timeout,
        )
        log.info("TCP: conectado a %s:%d", self._host, self._port)

    async def disconnect(self) -> None:
        if self._writer is not None:
            try:
                self._writer.close()
                await self._writer.wait_closed()
            except Exception:
                pass
        self._reader = None
        self._writer = None
        log.debug("TCP: desconectado de %s:%d", self._host, self._port)

    async def send(self, data: bytes) -> None:
        if self._writer is None or self._writer.is_closing():
            raise ConnectionError("TCP: nao conectado")
        self._writer.write(data)
        await self._writer.drain()

    async def recv(self, n: int = 4096) -> bytes:
        if self._reader is None:
            return b""
        while True:
            try:
                data = await asyncio.wait_for(
                    self._reader.read(n),
                    timeout=self._read_timeout,
                )
                if not data:
                    log.debug("TCP: EOF recebido de %s:%d", self._host, self._port)
                return data
            except asyncio.TimeoutError:
                log.debug(
                    "TCP: recv timeout sem dados de %s:%d - mantendo conexao",
                    self._host,
                    self._port,
                )
            except (ConnectionAbortedError, ConnectionResetError, OSError) as exc:
                log.debug("TCP: recv error: %s", exc)
                return b""

    @property
    def is_connected(self) -> bool:
        return (
            self._writer is not None
            and not self._writer.is_closing()
            and self._reader is not None
            and not self._reader.at_eof()
        )

    @property
    def description(self) -> str:
        return f"TCP {self._host}:{self._port}"
