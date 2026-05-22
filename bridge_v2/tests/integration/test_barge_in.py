"""Integration tests — Fase 7: barge-in durante SPEAKING/THINKING.

Verifica:
  - Barge-in via VOICE_ACTIVITY_START (firmware-driven)
  - Barge-in via VAD secundário (energy-driven)
  - Cancelamento da Task de turno
  - Transição INTERRUPTED → LISTENING
  - Reset de baseline (EXPR NEUTRAL + gaze central)
  - Métrica interruption_cancel_ms registrada
  - Invariante I-5: novo turn_id após barge-in
"""
from __future__ import annotations

import asyncio
import struct
from unittest.mock import AsyncMock, MagicMock

import pytest

from bridgev2.runtime.bus import EventBus
from bridgev2.runtime.events import (
    FirmwareConnected, VoiceActivityStart, VoiceActivityEnd,
    AudioChunkIn, FinalTranscript, BargeInDetected,
    SpeechDone, SentenceReady, IntentResolved,
    TranscriptQuality,
)
from bridgev2.runtime.orchestrator import Orchestrator
from bridgev2.runtime.turn_manager import TurnState
from bridgev2.audio.vad import BargeInMonitor, DEFAULT_THRESHOLD_RMS


def _make_audio(n_chunks: int = 50, rms: float = 300.0) -> list[bytes]:
    """Gera lista de chunks PCM com RMS target."""
    value = int(rms)
    chunk = struct.pack("<256h", *([value] * 256))
    return [chunk] * n_chunks


def _silence_chunks(n: int = 10) -> list[bytes]:
    return [bytes(512)] * n


async def _run_orch(orch: Orchestrator, bus: EventBus, events, timeout: float = 2.0):
    """Publica eventos no bus e drena o orchestrator."""
    async def _publisher():
        for evt in events:
            await bus.publish(evt)
            await asyncio.sleep(0)
        await asyncio.sleep(0.1)
        from bridgev2.runtime.events import ShutdownRequested
        await bus.publish(ShutdownRequested())

    await asyncio.gather(
        asyncio.wait_for(orch.run(), timeout=timeout),
        _publisher(),
        return_exceptions=True,
    )


class TestBargeInViaFirmwareEvent:
    """Barge-in disparado por VOICE_ACTIVITY_START do firmware."""

    @pytest.mark.asyncio
    async def test_voice_start_during_speaking_triggers_barge_in(self):
        bus = EventBus(default_maxsize=256)
        adapter = MagicMock()
        adapter.send_expr = AsyncMock()
        adapter.send_gaze = AsyncMock()
        adapter.send_speech_cancel = AsyncMock()
        adapter.send_say = AsyncMock()
        adapter.send_text_scroll = AsyncMock()
        adapter.send_emot_event = AsyncMock()
        adapter.send_action = AsyncMock()
        adapter.peer_supports = MagicMock(return_value=False)

        orch = Orchestrator(bus, get_adapter=lambda: adapter)

        events = [
            FirmwareConnected(),
            VoiceActivityStart(),
            *[AudioChunkIn(pcm=bytes(512)) for _ in range(40)],
            VoiceActivityEnd(),
            FinalTranscript(turn_id=1, text="olá"),
            # Dá tempo ao orchestrator processar o intent e ir para SPEAKING
            VoiceActivityStart(),  # barge-in
        ]

        await _run_orch(orch, bus, events)

        # Após barge-in, FSM deve estar em LISTENING (pronto para novo turno)
        assert orch._fsm.state in (TurnState.LISTENING, TurnState.IDLE)

    @pytest.mark.asyncio
    async def test_barge_in_records_interruption_metric(self):
        bus = EventBus(default_maxsize=256)
        adapter = MagicMock()
        adapter.send_expr = AsyncMock()
        adapter.send_gaze = AsyncMock()
        adapter.send_speech_cancel = AsyncMock()
        adapter.send_say = AsyncMock()
        adapter.send_text_scroll = AsyncMock()
        adapter.send_emot_event = AsyncMock()
        adapter.send_action = AsyncMock()
        adapter.peer_supports = MagicMock(return_value=False)

        orch = Orchestrator(bus, get_adapter=lambda: adapter)

        # Injeta BargeInDetected diretamente (simulando SPEAKING)
        orch._fsm.try_transition(TurnState.LISTENING)
        orch._fsm.try_transition(TurnState.COMMITTING_TURN)
        orch._fsm.try_transition(TurnState.THINKING)
        orch._fsm.try_transition(TurnState.SPEAKING)  # THINKING → SPEAKING válido
        from bridgev2.runtime.session import SessionContext
        orch._session = SessionContext(turn_id=5)

        await orch._on_barge_in(BargeInDetected(turn_id=5))

        snap = orch.metrics.snapshot_flat()
        assert "interruption_cancel_ms_p50" in snap
        assert snap["interruption_cancel_ms_p50"] >= 0.0

    @pytest.mark.asyncio
    async def test_barge_in_resets_baseline(self):
        bus = EventBus(default_maxsize=256)
        adapter = MagicMock()
        adapter.send_expr = AsyncMock()
        adapter.send_gaze = AsyncMock()
        adapter.send_speech_cancel = AsyncMock()
        adapter.send_say = AsyncMock()
        adapter.send_text_scroll = AsyncMock()
        adapter.send_emot_event = AsyncMock()
        adapter.send_action = AsyncMock()
        adapter.peer_supports = MagicMock(return_value=False)

        orch = Orchestrator(bus, get_adapter=lambda: adapter)

        orch._fsm.try_transition(TurnState.LISTENING)
        orch._fsm.try_transition(TurnState.COMMITTING_TURN)
        orch._fsm.try_transition(TurnState.THINKING)
        orch._fsm.try_transition(TurnState.SPEAKING)
        from bridgev2.runtime.session import SessionContext
        orch._session = SessionContext(turn_id=3)

        await orch._on_barge_in(BargeInDetected(turn_id=3))

        # reset_baseline envia EXPR NEUTRAL (expression_id=2) + gaze (0,0)
        expr_calls = [
            c for c in adapter.send_expr.call_args_list
            if c.args[0] == 2  # NEUTRAL
        ]
        assert len(expr_calls) >= 1
        adapter.send_gaze.assert_called()

    @pytest.mark.asyncio
    async def test_barge_in_transitions_to_listening(self):
        bus = EventBus(default_maxsize=256)
        adapter = MagicMock()
        adapter.send_expr = AsyncMock()
        adapter.send_gaze = AsyncMock()
        adapter.send_speech_cancel = AsyncMock()
        adapter.send_say = AsyncMock()
        adapter.send_text_scroll = AsyncMock()
        adapter.send_emot_event = AsyncMock()
        adapter.send_action = AsyncMock()
        adapter.peer_supports = MagicMock(return_value=False)

        orch = Orchestrator(bus, get_adapter=lambda: adapter)

        orch._fsm.try_transition(TurnState.LISTENING)
        orch._fsm.try_transition(TurnState.COMMITTING_TURN)
        orch._fsm.try_transition(TurnState.THINKING)
        from bridgev2.runtime.session import SessionContext
        orch._session = SessionContext(turn_id=7)

        old_turn_id = orch._session.turn_id
        await orch._on_barge_in(BargeInDetected(turn_id=7))

        # Deve estar em LISTENING com novo turn_id
        assert orch._fsm.state == TurnState.LISTENING
        assert orch._session is not None
        assert orch._session.turn_id != old_turn_id  # Invariante I-5
        assert orch._fsm.current_turn_id == orch._session.turn_id

    @pytest.mark.asyncio
    async def test_stale_barge_in_does_not_cancel_current_turn(self):
        bus = EventBus(default_maxsize=256)
        adapter = MagicMock()
        adapter.send_expr = AsyncMock()
        adapter.send_gaze = AsyncMock()
        adapter.send_speech_cancel = AsyncMock()
        adapter.send_say = AsyncMock()
        adapter.send_text_scroll = AsyncMock()
        adapter.send_emot_event = AsyncMock()
        adapter.send_action = AsyncMock()

        orch = Orchestrator(bus, get_adapter=lambda: adapter)

        orch._fsm.try_transition(TurnState.LISTENING, turn_id=20)
        orch._fsm.try_transition(TurnState.COMMITTING_TURN, turn_id=20)
        orch._fsm.try_transition(TurnState.THINKING, turn_id=20)
        from bridgev2.runtime.session import SessionContext
        orch._session = SessionContext(turn_id=20)

        await orch._on_barge_in(BargeInDetected(turn_id=19))

        assert orch._fsm.state == TurnState.THINKING
        assert orch._session.turn_id == 20
        adapter.send_speech_cancel.assert_not_awaited()

    @pytest.mark.asyncio
    async def test_barge_in_sends_speech_cancel_when_supported(self):
        bus = EventBus(default_maxsize=256)
        adapter = MagicMock()
        adapter.send_expr = AsyncMock()
        adapter.send_gaze = AsyncMock()
        adapter.send_speech_cancel = AsyncMock()
        adapter.send_say = AsyncMock()
        adapter.send_text_scroll = AsyncMock()
        adapter.send_emot_event = AsyncMock()
        adapter.send_action = AsyncMock()
        adapter.peer_supports = MagicMock(return_value=True)

        orch = Orchestrator(bus, get_adapter=lambda: adapter)

        orch._fsm.try_transition(TurnState.LISTENING)
        orch._fsm.try_transition(TurnState.COMMITTING_TURN)
        orch._fsm.try_transition(TurnState.THINKING)
        orch._fsm.try_transition(TurnState.SPEAKING)
        from bridgev2.runtime.session import SessionContext
        orch._session = SessionContext(turn_id=9)

        await orch._on_barge_in(BargeInDetected(turn_id=9))

        adapter.send_speech_cancel.assert_called_once_with(9)

    @pytest.mark.asyncio
    async def test_barge_in_ignored_when_not_interruptible(self):
        bus = EventBus(default_maxsize=256)
        orch = Orchestrator(bus)

        # IDLE não é interruptível
        assert orch._fsm.state == TurnState.IDLE
        await orch._on_barge_in(BargeInDetected(turn_id=1))
        # Deve continuar IDLE sem erro
        assert orch._fsm.state == TurnState.IDLE

    @pytest.mark.asyncio
    async def test_barge_in_vad_reset_after_trigger(self):
        bus = EventBus(default_maxsize=256)
        adapter = MagicMock()
        adapter.send_expr = AsyncMock()
        adapter.send_gaze = AsyncMock()
        adapter.send_speech_cancel = AsyncMock()
        adapter.send_say = AsyncMock()
        adapter.send_text_scroll = AsyncMock()
        adapter.send_emot_event = AsyncMock()
        adapter.send_action = AsyncMock()
        adapter.peer_supports = MagicMock(return_value=False)

        orch = Orchestrator(bus, get_adapter=lambda: adapter)
        orch._vad._above_count = 8  # simula contagem acumulada

        orch._fsm.try_transition(TurnState.LISTENING)
        orch._fsm.try_transition(TurnState.COMMITTING_TURN)
        orch._fsm.try_transition(TurnState.THINKING)
        orch._fsm.try_transition(TurnState.SPEAKING)
        from bridgev2.runtime.session import SessionContext
        orch._session = SessionContext(turn_id=2)

        await orch._on_barge_in(BargeInDetected(turn_id=2))

        # VAD deve ter sido resetado
        assert orch._vad.above_count == 0


class TestVadDrivenBargeIn:
    """Barge-in detectado pelo VAD secundário do bridge (sem evento do firmware)."""

    @pytest.mark.asyncio
    async def test_sustained_audio_during_speaking_triggers_barge_in(self):
        """Chunks com energia alta durante SPEAKING devem acionar barge-in via VAD."""
        bus = EventBus(default_maxsize=256)
        adapter = MagicMock()
        adapter.send_expr = AsyncMock()
        adapter.send_gaze = AsyncMock()
        adapter.send_speech_cancel = AsyncMock()
        adapter.send_say = AsyncMock()
        adapter.send_text_scroll = AsyncMock()
        adapter.send_emot_event = AsyncMock()
        adapter.send_action = AsyncMock()
        adapter.peer_supports = MagicMock(return_value=False)

        orch = Orchestrator(bus, get_adapter=lambda: adapter)

        # Configura VAD com sustain=2 para o teste ser rápido
        orch._vad = BargeInMonitor(threshold_rms=100, sustain_chunks=2)

        # Coloca FSM em SPEAKING
        orch._fsm.try_transition(TurnState.LISTENING)
        orch._fsm.try_transition(TurnState.COMMITTING_TURN)
        orch._fsm.try_transition(TurnState.THINKING)
        orch._fsm.try_transition(TurnState.SPEAKING)
        from bridgev2.runtime.session import SessionContext
        orch._session = SessionContext(turn_id=4)

        # Publica eventos de áudio com energia alta
        loud_chunk = struct.pack("<256h", *([1000] * 256))
        barge_events = [AudioChunkIn(pcm=loud_chunk) for _ in range(3)]

        barge_in_published = []
        sub = bus.subscribe(maxsize=32)

        async def _collector():
            async for evt in EventBus.iter_queue(sub):
                if isinstance(evt, BargeInDetected):
                    barge_in_published.append(evt)
                    break

        collect_task = asyncio.create_task(_collector())

        for evt in barge_events:
            await orch._on_audio_chunk(evt)
            await asyncio.sleep(0)

        await asyncio.wait_for(collect_task, timeout=1.0)
        assert len(barge_in_published) >= 1
        assert barge_in_published[0].turn_id == 4

    @pytest.mark.asyncio
    async def test_silence_during_speaking_does_not_trigger(self):
        bus = EventBus(default_maxsize=256)
        orch = Orchestrator(bus)

        orch._fsm.try_transition(TurnState.LISTENING)
        orch._fsm.try_transition(TurnState.COMMITTING_TURN)
        orch._fsm.try_transition(TurnState.THINKING)
        orch._fsm.try_transition(TurnState.SPEAKING)
        from bridgev2.runtime.session import SessionContext
        orch._session = SessionContext(turn_id=6)

        barge_events = []
        sub = bus.subscribe(maxsize=32)

        silence_chunk = bytes(512)
        for _ in range(20):
            await orch._on_audio_chunk(AudioChunkIn(pcm=silence_chunk))

        # Drena a fila do subscriber para ver se BargeInDetected chegou
        try:
            while True:
                evt = sub.get_nowait()
                if isinstance(evt, BargeInDetected):
                    barge_events.append(evt)
        except asyncio.QueueEmpty:
            pass

        assert len(barge_events) == 0

    @pytest.mark.asyncio
    async def test_vad_not_active_during_listening(self):
        """VAD de barge-in não deve disparar enquanto em LISTENING (só em can_interrupt)."""
        bus = EventBus(default_maxsize=256)
        orch = Orchestrator(bus)
        orch._vad = BargeInMonitor(threshold_rms=10, sustain_chunks=1)

        orch._fsm.try_transition(TurnState.LISTENING)
        from bridgev2.runtime.session import SessionContext
        orch._session = SessionContext(turn_id=8)

        barge_events = []
        sub = bus.subscribe(maxsize=32)

        loud_chunk = struct.pack("<256h", *([5000] * 256))
        for _ in range(5):
            await orch._on_audio_chunk(AudioChunkIn(pcm=loud_chunk))

        try:
            while True:
                evt = sub.get_nowait()
                if isinstance(evt, BargeInDetected):
                    barge_events.append(evt)
        except asyncio.QueueEmpty:
            pass

        assert len(barge_events) == 0  # LISTENING não é interruptível via VAD
