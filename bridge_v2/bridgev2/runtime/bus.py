"""bridgev2.runtime.bus — Bus de eventos async interno.

O bus distribui eventos tipados para assinantes registrados.
Cada assinante recebe sua própria asyncio.Queue (bounded por padrão).
Eventos de safety têm fila separada (prioridade) e nunca são bloqueados
por backpressure normal.

Uso:
    bus = EventBus()
    sub = bus.subscribe()          # Queue[Any], recebe todos os eventos
    sub = bus.subscribe(FooEvent)  # Queue[FooEvent], recebe só FooEvent
    await bus.publish(FooEvent(...))
    event = await sub.get()
"""
from __future__ import annotations

import asyncio
import logging
from typing import Any

log = logging.getLogger(__name__)

_SENTINEL = object()  # objeto único para sinalizar fechamento da fila


class EventBus:
    """Bus de eventos async com suporte a filtro por tipo."""

    def __init__(self, default_maxsize: int = 128) -> None:
        self._default_maxsize = default_maxsize
        # lista de (tipos filtrados, queue); tipos=() significa "todos"
        self._subscribers: list[tuple[tuple[type, ...], asyncio.Queue]] = []
        self._closed = False

    # ── Assinatura ─────────────────────────────────────────────────────────

    def subscribe(
        self,
        *event_types: type,
        maxsize: int = 0,
    ) -> asyncio.Queue:
        """Retorna uma Queue que recebe eventos.

        Se event_types for vazio, recebe todos os eventos.
        Se fornecidos, recebe apenas eventos desses tipos.
        maxsize=0 usa o default do bus; maxsize=-1 = ilimitado.
        """
        sz = maxsize if maxsize != 0 else self._default_maxsize
        if sz < 0:
            sz = 0  # asyncio.Queue(0) = ilimitado
        q: asyncio.Queue = asyncio.Queue(maxsize=sz)
        self._subscribers.append((event_types, q))
        return q

    def unsubscribe(self, q: asyncio.Queue) -> None:
        self._subscribers = [(types, sq) for types, sq in self._subscribers if sq is not q]

    # ── Publicação ─────────────────────────────────────────────────────────

    async def publish(self, event: Any) -> None:
        """Publica um evento para todos os assinantes elegíveis.

        Usa put_nowait() com fallback de log — nunca bloqueia o loop.
        Assinantes lentos descartam eventos quando a fila está cheia
        (backpressure: o produtor é quem deve tratar isso).
        """
        if self._closed:
            return
        for event_types, q in self._subscribers:
            if event_types and not isinstance(event, event_types):
                continue
            try:
                q.put_nowait(event)
            except asyncio.QueueFull:
                log.warning(
                    "bus.publish: fila cheia para %s — evento %s descartado",
                    type(event).__name__,
                    type(event).__name__,
                )

    def publish_nowait(self, event: Any) -> None:
        """Versão síncrona — segura para chamar de callbacks não-async."""
        if self._closed:
            return
        for event_types, q in self._subscribers:
            if event_types and not isinstance(event, event_types):
                continue
            try:
                q.put_nowait(event)
            except asyncio.QueueFull:
                log.warning(
                    "bus.publish_nowait: fila cheia — %s descartado",
                    type(event).__name__,
                )

    # ── Fechamento ─────────────────────────────────────────────────────────

    async def close(self) -> None:
        """Envia sentinel para todos os assinantes e marca bus como fechado."""
        self._closed = True
        for _, q in self._subscribers:
            try:
                q.put_nowait(_SENTINEL)
            except asyncio.QueueFull:
                pass
        self._subscribers.clear()

    # ── Iterador assíncrono conveniente ────────────────────────────────────

    @staticmethod
    async def iter_queue(q: asyncio.Queue):
        """Itera sobre uma Queue até receber o sentinel de fechamento."""
        while True:
            event = await q.get()
            if event is _SENTINEL:
                q.task_done()  # sentinel também precisa de task_done
                break
            yield event
            q.task_done()

    @property
    de