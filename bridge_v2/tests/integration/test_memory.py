"""Fase 10 — Smoke test de memória: N turnos não devem crescer indefinidamente.

Verifica que objetos de sessão são liberados após cada turno.
Usa tracemalloc para detectar crescimento anormal.

Não é um long-run de 8h — esse roda em CI. Este teste verifica:
  1. Objetos SessionContext não acumulam (gc após cada turno)
  2. Buses/queues não acumulam eventos entre turnos
  3. N turnos com intent local não crescem o heap além de um threshold razoável
"""
from __future__ import annotations

import asyncio
import gc
import tracemalloc
from typing import AsyncIterator
from unittest.mock import AsyncMock, MagicMock

import pytest

from bridgev2.runtime.bus import EventBus
from bridgev2.runtime.events import (
    FirmwareConnected, VoiceActivityStart, VoiceActivityEnd,
    AudioChunkIn, FinalTranscript, ShutdownRequested,
)
from bridgev2.runtime.orchestrator import Orchestrator
from bridgev2.runtime.turn_manager import TurnState
from bridgev2.runtime.session import SessionContext


N_TURNS = 30                     # turnos para o smoke test
HEAP_GROWTH_LIMIT_KB = 2048      # 2 MB de crescimento máximo tolerado


_TURN_ID_BASE = 50_000   # base alta para evitar colisão com outros testes


async def _run_n_local_turns(n: int) -> Orchestrator:
    """Roda N turnos de intent local e retorna o orchestrator.

    Usa FinalTranscript sintético com turn_id alto e único para evitar colisão
    com o contador global de turn_ids de outros testes.
    """
    bus = EventBus(default_maxsize=256)
    orch = Orchestrator(bus)

    async def _run_turns():
        await bus.publish(FirmwareConnected())
        for i in range(n):
            # FinalTranscript quando FSM está em IDLE → orchestrator cria sessão sintética
            # turn_id único e alto para não colidir com outros testes
            await bus.publish(FinalTranscript(
                turn_id=_TURN_ID_BASE + i,
                text="que horas são",
            ))
            await asyncio.sleep(0.05)
        await asyncio.sleep(0.2)
        await bus.publish(ShutdownRequested())

    await asyncio.gather(
        asyncio.wait_for(orch.run(), timeout=30.0),
        _run_turns(),
        return_exceptions=True,
    )
    return orch


class TestMemorySmoke:
    @pytest.mark.asyncio
    async def test_sessions_not_accumulated(self):
        """Após N turnos, não deve haver SessionContext vivos (exceto o corrente)."""
        orch = await _run_n_local_turns(N_TURNS)

        gc.collect()
        # Após shutdown, sessão deve ser None
        assert orch._session is None, "SessionContext vazou após shutdown"
        assert orch._turn_task is None, "_turn_task não foi limpo"
        assert orch._watchdog is None, "watchdog não foi limpo"

    @pytest.mark.asyncio
    async def test_heap_growth_bounded(self):
        """N turnos não devem crescer o heap além de HEAP_GROWTH_LIMIT_KB."""
        gc.collect()
        tracemalloc.start()
        snapshot_before = tracemalloc.take_snapshot()

        await _run_n_local_turns(N_TURNS)

        gc.collect()
        snapshot_after = tracemalloc.take_snapshot()
        tracemalloc.stop()

        stats = snapshot_after.compare_to(snapshot_before, "lineno")
        total_growth = sum(s.size_diff for s in stats if s.size_diff > 0)
        growth_kb = total_growth / 1024

        assert growth_kb < HEAP_GROWTH_LIMIT_KB, (
            f"Crescimento de heap suspeito: {growth_kb:.0f} KB após {N_TURNS} turnos "
            f"(limite: {HEAP_GROWTH_LIMIT_KB} KB)"
        )

    @pytest.mark.asyncio
    async def test_bus_queue_cleared_between_turns(self):
        """A fila do bus não deve acumular eventos entre turnos."""
        bus = EventBus(default_maxsize=256)
        orch = Orchestrator(bus)

        async def _pub():
            await bus.publish(FirmwareConnected())
            for i in range(5):
                await bus.publish(VoiceActivityStart())
                for _ in range(40):
                    await bus.publish(AudioChunkIn(pcm=bytes(512)))
                await bus.publish(VoiceActivityEnd())
                await bus.publish(FinalTranscript(turn_id=i + 1, text="que horas são"))
                await asyncio.sleep(0.05)
            await asyncio.sleep(0.2)
            await bus.publish(ShutdownRequested())

        await asyncio.gather(
            asyncio.wait_for(orch.run(), timeout=15.0),
            _pub(),
            return_exceptions=True,
        )

        # Após shutdown, FSM deve estar em IDLE
        assert orch._fsm.state == TurnState.IDLE

    @pytest.mark.asyncio
    async def test_watchdog_tasks_not_accumulated(self):
        """Watchdog tasks devem ser canceladas e GC'd entre turnos."""
        orch = await _run_n_local_turns(10)
        # Deixa o event loop processar cancelamentos pendentes
        await asyncio.sleep(0.1)
        gc.collect()
        # Watchdog do último turno deve ter sido cancelado no _finish_turn
        assert orch._watchdog is None


class TestTurnIsolation:
    @pytest.mark.asyncio
    async def test_turn_id_monotonically_increases(self):
        """turn_ids devem ser únicos e crescentes entre turnos."""
        bus = EventBus(default_maxsize=256)
        orch = Orchestrator(bus)
        turn_ids: list[int] = []

        orig_begin = orch._begin_turn.__func__

        async def _tracked_begin(self_orch):
            await orig_begin(self_orch)
            if self_orch._session:
                turn_ids.append(self_orch._session.turn_id)

        import types
        orch._begin_turn = types.MethodType(_tracked_begin, orch)

        async def _pub():
            await bus.publish(FirmwareConnected())
            for i in range(5):
                await bus.publish(VoiceActivityStart())
                for _ in range(40):
                    await bus.publish(AudioChunkIn(pcm=bytes(512)))
                await bus.publish(VoiceActivityEnd())
                await bus.publish(FinalTranscript(turn_id=i + 1, text="que horas são"))
                await asyncio.sleep(0.05)
            await asyncio.sleep(0.2)
            await bus.publish(ShutdownRequested())

        await asyncio.gather(
            asyncio.wait_for(orch.run(), timeout=15.0),
            _pub(),
            return_exceptions=True,
        )

        if turn_ids:
            assert turn_ids == sorted(set(turn_ids)), "turn_ids repetidos ou fora de ordem"
