"""Firmware connection supervisor with automatic reconnect."""

from __future__ import annotations

import asyncio
import logging
import random
from collections.abc import Callable
from typing import Any

from .adapter import FirmwareAdapter
from .base import Transport

log = logging.getLogger(__name__)


class ConnectionSupervisor:
    """Manage firmware connection lifecycle and reconnect backoff."""

    def __init__(
        self,
        transport_factory: Callable[[], Transport],
        bus: Any,
        reconnect: Any,
    ) -> None:
        self._transport_factory = transport_factory
        self._bus = bus
        self._reconnect = reconnect
        self._adapter: FirmwareAdapter | None = None
        self._shutdown = False
        self._attempt = 0

    @property
    def adapter(self) -> FirmwareAdapter | None:
        return self._adapter

    @property
    def is_connected(self) -> bool:
        return self._adapter is not None and self._adapter.is_connected

    async def run(self) -> None:
        """Connect, run the adapter, and reconnect with backoff on disconnect."""
        log.info("ConnectionSupervisor: iniciando")
        delay = self._reconnect.delay_s

        while not self._shutdown:
            self._attempt += 1
            transport = self._transport_factory()
            log.info(
                "ConnectionSupervisor: tentativa #%d - %s",
                self._attempt,
                transport.description,
            )

            try:
                await transport.connect()
            except asyncio.CancelledError:
                break
            except Exception as exc:
                log.warning(
                    "ConnectionSupervisor: falha na conexao: %s - aguardando %.1f s",
                    exc,
                    delay,
                )
                await self._sleep(delay)
                delay = self._next_delay(delay)
                continue

            self._adapter = FirmwareAdapter(transport, self._bus)
            delay = self._reconnect.delay_s

            try:
                await self._adapter.run()
            except asyncio.CancelledError:
                await transport.disconnect()
                break
            except Exception as exc:
                log.warning("ConnectionSupervisor: adapter encerrou: %s", exc)
            finally:
                self._adapter = None
                try:
                    await transport.disconnect()
                except Exception:
                    pass

            if self._shutdown:
                break

            log.info(
                "ConnectionSupervisor: desconectado. reconectando em %.1f s...",
                delay,
            )
            await self._sleep(delay)
            delay = self._next_delay(delay)

        log.info("ConnectionSupervisor: encerrado apos %d tentativas", self._attempt)

    async def shutdown(self) -> None:
        self._shutdown = True

    async def _sleep(self, seconds: float) -> None:
        jitter = random.uniform(0, seconds * 0.1)
        try:
            await asyncio.sleep(seconds + jitter)
        except asyncio.CancelledError:
            pass

    def _next_delay(self, current: float) -> float:
        return min(current * 2.0, self._reconnect.max_delay_s)
