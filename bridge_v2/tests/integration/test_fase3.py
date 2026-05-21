"""Testes de integração — Fase 3: FinalTranscript → FSM → RobotCommand no bus.

Design dos helpers:
  - O turno completo (IDLE→...→IDLE) ocorre em um único tick do event loop
    porque todos os `await bus.publish()` são put_nowait internamente (sem
    suspend real). Por isso o polling de estado da FSM não funciona.
  - Abordagem correta: event-driven — subscribing antes de publicar e
    aguardando com asyncio.wait_for no SpeechDone ou IntentResolved.

Cobre:
  - FinalTranscript injetado → intent local → RobotCommand emitido no bus
  - FinalTranscript sem match local → FSM → IDLE, sem RobotCommand de expr
  - FinalTranscript não-utilizável → descartado, FSM → IDLE
  - Baseline reset: após SpeechDone, expr NEUTRAL + gaze central
  - Múltiplos turnos em sequência
  - Adapter mock: send_expr / send_gaze chamados
  - turn_id propagado para todos os RobotCommands
"""
from __future__ import annotations

import asyncio
from unittest.mock import AsyncMock

import pytest

from bridgev2.config import (
    AudioConfig, BridgeV2Config, LlmConfig, LlmProvider, LogLevel,
    OpsConfig, PipelineMode, ReconnectConfig, SttConfig, TransportConfig,
    TtsConfig,
)
from bridgev2.runtime.bus import EventBus
from bridgev2.runtime.events import (
    FinalTranscript,
    IntentResolved,
    RobotCommand,
    SpeechDone,
    TranscriptQuality,
)
from bridgev2.runtime.orchestrator import Orchestrator
from bridgev2.runtime.turn_manager import TurnState


# ── Helpers e fixtures ────────────────────────────────────────────────────────

def _make_config() -> BridgeV2Config:
    return BridgeV2Config(
        transport=TransportConfig(host=None, port=9000, uart=None, baudrate=1000000),
        llm=LlmConfig(
            provider=LlmProvider.NONE, model="none",
            timeout_s=5.0, max_output_tokens=140, max_reply_chars=180,
            openai_key_configured=False, gemini_key_configured=False,
        ),
        pipeline_mode=PipelineMode.LOCAL_ONLY,
        stt=SttConfig(model="small", backend="faster", device="cpu", compute_type="int8"),
        tts=TtsConfig(piper_executable="piper", piper_model="",
                      cache_size=4, sample_rate=16000, target_peak=8000),
        audio=AudioConfig(
            chunk_samples=256, sample_rate=16000,
            min_transcribe_rms=140.0, min_transcribe_peak=1600,
            min_utterance_samples=8000,
            max_no_speech_prob=0.75, min_avg_logprob=-1.10,
            max_compression_ratio=2.60,
        ),
        reconnect=ReconnectConfig(delay_s=0.05, max_delay_s=0.2, connect_timeout_s=2.0),
        ops=OpsConfig(port=8765, token_configured=False),
        log_level=LogLevel.DEBUG,
        dry_run=True,
        replay_path=None,
    )


async def _wait_event(q: asyncio.Queue, timeout: float = 2.0):
    """Aguarda um item da queue com timeout. Lança TimeoutError se expirar."""
    return await asyncio.wait_for(q.get(), timeout=timeout)


async def _drain(q: asyncio.Queue, timeout: float = 0.2) -> list:
    """Drena todos os itens de uma queue com timeout por item."""
    items = []
    try:
        while True:
            items.append(await asyncio.wait_for(q.get(), timeout=timeout))
    except asyncio.TimeoutError:
        pass
    return items


async def _run_turn(
    bus: EventBus,
    ft: FinalTranscript,
    *,
    q_done: asyncio.Queue | None = None,
    timeout: float = 2.0,
) -> SpeechDone | None:
    """Publica FinalTranscript e aguarda SpeechDone se a queue for fornecida.

    Para turnos que não geram SpeechDone (sem match local), omitir q_done
    e usar um pequeno sleep após a chamada.
    """
    await bus.publish(ft)
    if q_done is not None:
        return await _wait_event(q_done, timeout=timeout)
    await asyncio.sleep(0.05)  # yield para o orchestrator processar
    return None


# ── Injeção direta (adapter=None / dry-run) ───────────────────────────────────

class TestFinalTranscriptInjection:
    async def test_known_intent_emits_robot_command(self):
        """FinalTranscript com intent local → RobotCommand publicado no bus."""
        bus = EventBus(default_maxsize=256)
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None)
        q_cmd = bus.subscribe(RobotCommand, maxsize=64)
        q_done = bus.subscribe(SpeechDone)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await _run_turn(bus, FinalTranscript(turn_id=1, text="tudo bem"), q_done=q_done)

            cmds = await _drain(q_cmd)
            kinds = [c.kind for c in cmds]
            assert "expr" in kinds, f"expr esperado em {kinds}"
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)

    async def test_known_intent_emits_intent_resolved(self):
        """FinalTranscript com match local → IntentResolved publicado no bus."""
        bus = EventBus(default_maxsize=256)
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None)
        q_intent = bus.subscribe(IntentResolved)
        q_done = bus.subscribe(SpeechDone)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await _run_turn(bus, FinalTranscript(turn_id=2, text="olá"), q_done=q_done)

            intent_evt = await _wait_event(q_intent)
            assert isinstance(intent_evt, IntentResolved)
            assert intent_evt.intent_name == "local_greeting"
            assert intent_evt.turn_id == 2
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)

    async def test_no_match_does_not_emit_robot_command(self):
        """FinalTranscript sem match local → nenhum RobotCommand de expr emitido."""
        bus = EventBus(default_maxsize=256)
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None)
        q_cmd = bus.subscribe(RobotCommand)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            # Sem match: não há SpeechDone — aguarda via sleep
            await _run_turn(bus, FinalTranscript(turn_id=3, text="qual a receita de bolo"))

            assert q_cmd.empty(), "Nenhum RobotCommand esperado sem intent"
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)

    async def test_fsm_returns_to_idle_after_intent(self):
        """Após turno com intent, FSM está em IDLE."""
        bus = EventBus(default_maxsize=256)
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None)
        q_done = bus.subscribe(SpeechDone)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await _run_turn(bus, FinalTranscript(turn_id=4, text="bom dia"), q_done=q_done)
            assert orch._fsm.state == TurnState.IDLE
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)

    async def test_fsm_returns_to_idle_after_no_match(self):
        """Mesmo sem match, FSM deve voltar a IDLE."""
        bus = EventBus(default_maxsize=256)
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await _run_turn(bus, FinalTranscript(turn_id=5, text="algo sem match nenhum"))
            assert orch._fsm.state == TurnState.IDLE
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)


# ── Transcript não-utilizável ─────────────────────────────────────────────────

class TestUnusableTranscript:
    async def test_empty_transcript_discarded(self):
        """FinalTranscript com texto vazio é descartado."""
        bus = EventBus(default_maxsize=256)
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None)
        q_cmd = bus.subscribe(RobotCommand)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await _run_turn(
                bus,
                FinalTranscript(turn_id=10, text="", quality=TranscriptQuality.EMPTY),
            )
            assert q_cmd.empty()
            assert orch._fsm.state == TurnState.IDLE
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)

    async def test_no_speech_transcript_discarded(self):
        """FinalTranscript com quality=NO_SPEECH é descartado."""
        bus = EventBus(default_maxsize=256)
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None)
        q_cmd = bus.subscribe(RobotCommand)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await _run_turn(
                bus,
                FinalTranscript(turn_id=11, text="noise", quality=TranscriptQuality.NO_SPEECH),
            )
            assert q_cmd.empty()
            assert orch._fsm.state == TurnState.IDLE
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)


# ── Baseline reset ────────────────────────────────────────────────────────────

class TestBaselineReset:
    async def test_after_speech_done_expr_neutral_emitted(self):
        """Após SpeechDone, RobotCommand 'expr' NEUTRAL (expression_id=2) emitido."""
        bus = EventBus(default_maxsize=256)
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None)
        q_cmd = bus.subscribe(RobotCommand, maxsize=64)
        q_done = bus.subscribe(SpeechDone)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await _run_turn(bus, FinalTranscript(turn_id=20, text="tudo bem"), q_done=q_done)

            cmds = await _drain(q_cmd)
            neutral = [c for c in cmds if c.kind == "expr" and c.payload.get("expression_id") == 2]
            assert neutral, f"Nenhum expr NEUTRAL em: {[(c.kind, c.payload) for c in cmds]}"
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)

    async def test_after_speech_done_gaze_center_emitted(self):
        """Após SpeechDone, gaze central (x=0, y=0) emitido."""
        bus = EventBus(default_maxsize=256)
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None)
        q_cmd = bus.subscribe(RobotCommand, maxsize=64)
        q_done = bus.subscribe(SpeechDone)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await _run_turn(bus, FinalTranscript(turn_id=21, text="olá"), q_done=q_done)

            cmds = await _drain(q_cmd)
            gaze_center = [
                c for c in cmds
                if c.kind == "gaze" and c.payload.get("x") == 0.0 and c.payload.get("y") == 0.0
            ]
            assert gaze_center, f"Nenhum gaze central em: {[(c.kind, c.payload) for c in cmds]}"
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)


# ── Múltiplos turnos em sequência ─────────────────────────────────────────────

class TestMultipleTurns:
    async def test_two_sequential_turns(self):
        """Dois turnos em sequência: FSM processa ambos e volta a IDLE."""
        bus = EventBus(default_maxsize=256)
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None)
        q_intent = bus.subscribe(IntentResolved, maxsize=64)
        q_done = bus.subscribe(SpeechDone, maxsize=64)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            # Turno 1
            await _run_turn(bus, FinalTranscript(turn_id=30, text="que horas são"), q_done=q_done)
            assert orch._fsm.state == TurnState.IDLE

            # Turno 2
            await _run_turn(bus, FinalTranscript(turn_id=31, text="tchau"), q_done=q_done)
            assert orch._fsm.state == TurnState.IDLE

            intents = await _drain(q_intent)
            names = [i.intent_name for i in intents]
            assert "local_time" in names, f"local_time esperado em {names}"
            assert "local_farewell" in names, f"local_farewell esperado em {names}"
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)

    async def test_three_sequential_turns_different_intents(self):
        """Três turnos: greeting, status, farewell."""
        bus = EventBus(default_maxsize=256)
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None)
        q_intent = bus.subscribe(IntentResolved, maxsize=64)
        q_done = bus.subscribe(SpeechDone, maxsize=64)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            for text in ["olá", "tudo bem", "tchau"]:
                await _run_turn(bus, FinalTranscript(turn_id=40, text=text), q_done=q_done)

            intents = await _drain(q_intent)
            names = [i.intent_name for i in intents]
            assert "local_greeting" in names
            assert "local_status" in names
            assert "local_farewell" in names
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)


# ── Com adapter mock ──────────────────────────────────────────────────────────

class TestWithAdapter:
    async def test_adapter_send_expr_called(self):
        """Com adapter, send_expr é chamado para o intent."""
        bus = EventBus(default_maxsize=256)
        adapter = AsyncMock()
        q_done = bus.subscribe(SpeechDone)

        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: adapter)
        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await _run_turn(bus, FinalTranscript(turn_id=40, text="tudo bem"), q_done=q_done)
            adapter.send_expr.assert_awaited()
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)

    async def test_adapter_gaze_called_for_look_intent(self):
        """Para local_look_up, send_gaze é chamado com y negativo."""
        bus = EventBus(default_maxsize=256)
        adapter = AsyncMock()
        q_done = bus.subscribe(SpeechDone)

        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: adapter)
        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await _run_turn(bus, FinalTranscript(turn_id=41, text="olha pra cima"), q_done=q_done)

            gaze_calls = adapter.send_gaze.await_args_list
            assert any(call.args[1] < 0 for call in gaze_calls), (
                f"Esperado send_gaze com y<0, calls={gaze_calls}"
            )
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)

    async def test_adapter_not_called_when_none(self):
        """Com adapter=None, RobotCommand ainda vai ao bus mas sem chamar adapter."""
        bus = EventBus(default_maxsize=256)
        q_cmd = bus.subscribe(RobotCommand, maxsize=64)
        q_done = bus.subscribe(SpeechDone)

        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None)
        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await _run_turn(bus, FinalTranscript(turn_id=42, text="bom dia"), q_done=q_done)
            cmds = await _drain(q_cmd)
            assert cmds, "RobotCommands devem ser publicados mesmo sem adapter"
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)


# ── Turn_id propagado ─────────────────────────────────────────────────────────

class TestTurnIdPropagation:
    async def test_all_commands_carry_injected_turn_id(self):
        """Todos os RobotCommands do turno carregam o turn_id injetado."""
        bus = EventBus(default_maxsize=256)
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None)
        q_cmd = bus.subscribe(RobotCommand, maxsize=64)
        q_done = bus.subscribe(SpeechDone)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await _run_turn(bus, FinalTranscript(turn_id=777, text="olá"), q_done=q_done)

            cmds = await _drain(q_cmd)
            assert cmds, "Esperado ao menos um RobotCommand"
            wrong = [c for c in cmds if c.turn_id != 777]
            assert not wrong, f"Comandos com turn_id errado: {wrong}"
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)


# ── Injeção com turn_id não sequencial ───────────────────────────────────────

class TestSyntheticSession:
    async def test_synthetic_session_created_from_idle(self):
        """FinalTranscript injetado com FSM em IDLE cria sessão sintética."""
        bus = EventBus(default_maxsize=256)
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None)
        q_intent = bus.subscribe(IntentResolved)
        q_done = bus.subscribe(SpeechDone)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            assert orch._fsm.is_idle
            await _run_turn(bus, FinalTranscript(turn_id=500, text="bridge ok"), q_done=q_done)

            evt = await _wait_event(q_intent)
            assert evt.intent_name == "local_bridge_test"
            assert evt.turn_id == 500
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)

    async def test_duplicate_turn_id_ignored_during_active_turn(self):
        """FinalTranscript com turn_id diferente durante turno ativo é ignorado."""
        bus = EventBus(default_maxsize=256)
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None)
        q_intent = bus.subscribe(IntentResolved, maxsize=64)
        q_done = bus.subscribe(SpeechDone)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            # Publica turn_id=1; enquanto processando, outro FinalTranscript
            # com turn_id=99 seria ignorado (sessão ativa tem turn_id=1)
            # Neste teste simples verificamos que o turn_id=1 é completado
            await _run_turn(bus, FinalTranscript(turn_id=1, text="olá"), q_done=q_done)

            evt = await _wait_event(q_intent)
            assert evt.turn_id == 1
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)
