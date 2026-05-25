"""bridgev2.transport.reconnect — ConnectionSupervisor: gerencia ciclo de vida da conexao.

Reconecta com backoff exponencial. Publica FirmwareConnected/Disconnected no bus.
Em operacao normal: conecta, inicia FirmwareAdapter.run(), aguarda desconexao.
"""
from __future__ import annotations

import asyncio
import logging
import math
import random
from typing import Callable

from .base import Transport
from .adapter import FirmwareAdapter
from ..runtime.bus import EventBus
from ..config import ReconnectConfig

log = logging.getLogger(__name__)


class ConnectionSupervisor:
    """Gerencia a conexao com o firmware com reconexao automatica.

    transport_factory: callable() -> Transport — criado a cada tentativa
    """

    def __init__(
        self,
        transport_factory: Callable[[], Transport],
        bus: EventBus,
        reconnect: ReconnectConfig,
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
        """Loop principal: conecta, aguarda desconexao, reconecta com backoff."""
        log.info("ConnectionSupervisor: iniciando")
        delay = self._reconnect.delay_s

        while not self._shutdown:
            self._attempt += 1
            transport = self._transport_factory()
            log.info(
                "ConnectionSupervisor: tentativa #%d — %s",
                self._attempt, transport.description,
            )

            try:
                await transport.connect()
            except asyncio.CancelledError:
                break
            except Exception as e:
                log.warning(
                    "ConnectionSupervisor: falha na conexao: %s — aguardando %.1f s",
                    e, delay,
                )
                await self._sleep(delay)
                delay = self._next_delay(delay)
                continue

            # Conexao estabelecida — cria adapter e roda
            self._adapter = FirmwareAdapter(transport, self._bus)
            delay = self._reconnect.delay_s  # reset do backoff em conexao bem-sucedida

            try:
                await self._adapter.run()
            except asyncio.CancelledError:
                await transport.disconnect()
                break
            except Exception as e:
                log.warning("ConnectionSupervisor: adapter encerrou: %s", e)
            finally:
                self._adapter = None
                try:
                    await transport.disconnect()
                except Exception:
                    pass

            if self._shutdown:
                break

            log.info(
                "ConnectionSupervisor: desconectado. reconectando em %.1f s...", delay
            )
            await self._sleep(delay)
            delay = self._next_delay(delay)

        log.info("ConnectionSupervisor: encerrado apos %d tentativas", self._attempt)

    async def shutdown(self) -> None:
        self._shutdown = True

    async def _sleep(self, seconds: float) -> None:
        """Sleep interruptivel com jitter pequeno."""
        jitter = random.uniform(0, seconds * 0.1)
        try:
            await asyncio.sleep(seconds + jitter)
        except asyncio.CancelledError:
            pass

    def _next_delay(self, current: float) -> float:
        """Backoff exponencial com teto."""
        return min(current * 2.0, self._reconnect.max_delay_s)
