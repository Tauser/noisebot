"""Testes de integração para o caminho LLM (Fase 5) do Orchestrator.

Padrão event-driven: subscribe SpeechDone/TurnError antes de publicar
FinalTranscript, usa asyncio.wait_for para não pender.

Critério de aceite (Fase 5): queda de API → fallback local sem travar o turno.
"""
from __future__ import annotations

import asyncio
from typing import AsyncIterator

import pytest
import pytest_asyncio

from bridgev2.runtime.bus import EventBus
from bridgev2.runtime.events import (
    FinalTranscript,
    LlmReplyComplete,
    LlmTokenDelta,
    SentenceReady,
    SpeechDone,
    TurnError,
    TranscriptQuality,
    IntentResolved,
)
from bridgev2.runtime.orchestrator import Orchestrator
from bridgev2.llm.circuit_breaker import CircuitOpenError


# ── Mock LLM providers ────────────────────────────────────────────────────────

class MockStreamingLLM:
    """Streaming mock: emite tokens de um JSON de resposta."""

    _provider_name = "mock"
    _model = "mock-model"

    def __init__(
        self,
        reply: str = "Olá! Posso ajudar.",
        expression_id: int = 1,
        action: int = 0,
        emot_event: int = 2,
        raise_exc: Exception | None = None,
        tokens: list[str] | None = None,
    ) -> None:
        self._raise_exc = raise_exc
        if tokens is not None:
            self._tokens = tokens
        else:
            raw = (
                f'{{"reply":"{reply}",'
                f'"expression_id":{expression_id},'
                f'"action":{action},'
                f'"emot_event":{emot_event}}}'
            )
            # Emite em pedaços de ~10 chars para simular streaming
            self._tokens = [raw[i:i+10] for i in range(0, len(raw), 10)]

    def generate_stream(self, text: str, context: dict) -> AsyncIterator[str]:
        return self._do_stream(text, context)

    async def _do_stream(self, text: str, context: dict):
        if self._raise_exc is not None:
            raise self._raise_exc
        for token in self._tokens:
            yield token


class MockBatchLLM:
    """Batch mock: retorna resposta completa em generate_stream (sem streaming real)."""

    _provider_name = "mock_batch"
    _model = "mock-batch"

    def __init__(self, reply: str = "Resposta batch.") -> None:
        self._reply = reply
        raw = f'{{"reply":"{reply}","expression_id":0,"action":1,"emot_event":2}}'
        self._tokens = [raw]

    def generate_stream(self, text: str, context: dict) -> AsyncIterator[str]:
        return self._do_stream(text, context)

    async def _do_stream(self, text: str, context: dict):
        for token in self._tokens:
            yield token


# ── Helpers ───────────────────────────────────────────────────────────────────

def _good_transcript(turn_id: int = 1, text: str = "me conta uma historia") -> FinalTranscript:
    """FinalTranscript com qualidade GOOD (não corresponde a nenhum intent local)."""
    return FinalTranscript(
        turn_id=turn_id,
        text=text,
        quality=TranscriptQuality.GOOD,
    )


async def _wait_event(q: asyncio.Queue, timeout: float = 3.0):
    return await asyncio.wait_for(q.get(), timeout=timeout)


async def _run_llm_turn(
    bus: EventBus,
    ft: FinalTranscript,
    wait_for_speech_done: bool = True,
    timeout: float = 3.0,
) -> tuple[list, asyncio.Queue]:
    """Publica FinalTranscript e coleta eventos até SpeechDone ou TurnError."""
    q: asyncio.Queue = asyncio.Queue()
    collected: list = []

    async def _collector(event):
        collected.append(event)
        if isinstance(event, (SpeechDone, TurnError)):
            await q.put(event)

    sub = bus.subscribe(maxsize=100)

    async def _drain():
        async for event in EventBus.iter_queue(sub):
            await _collector(event)

    drain_task = asyncio.create_task(_drain())

    await bus.publish(ft)

    if wait_for_speech_done:
        try:
            final_event = await asyncio.wait_for(q.get(), timeout=timeout)
        except asyncio.TimeoutError:
            final_event = None
        finally:
            drain_task.cancel()
            try:
                await drain_task
            except (asyncio.CancelledError, Exception):
                pass
        return collected, final_event
    return collected, q


# ── Fixtures ──────────────────────────────────────────────────────────────────

@pytest_asyncio.fixture
async def bus():
    b = EventBus()
    yield b
    await b.close()


def _make_orch(bus: EventBus, llm=None) -> Orchestrator:
    return Orchestrator(bus=bus, llm_provider=llm)


# ── Testes do caminho LLM ─────────────────────────────────────────────────────

class TestLLMPath:
    @pytest.mark.asyncio
    async def test_llm_turn_reaches_speech_done(self, bus):
        """Turno LLM completo emite SpeechDone."""
        llm = MockStreamingLLM(reply="Estou bem, obrigado!")
        orch = _make_orch(bus, llm)
        asyncio.create_task(orch.run())
        await asyncio.sleep(0)

        _, final = await _run_llm_turn(bus, _good_transcript())
        assert isinstance(final, SpeechDone)

    @pytest.mark.asyncio
    async def test_llm_turn_publishes_llm_reply_complete(self, bus):
        """Turno LLM publica LlmReplyComplete com reply correto."""
        llm = MockStreamingLLM(reply="Aqui está minha resposta para você!")
        orch = _make_orch(bus, llm)
        asyncio.create_task(orch.run())
        await asyncio.sleep(0)

        collected, _ = await _run_llm_turn(bus, _good_transcript())
        replies = [e for e in collected if isinstance(e, LlmReplyComplete)]
        assert len(replies) == 1
        assert "Aqui" in replies[0].reply

    @pytest.mark.asyncio
    async def test_llm_turn_publishes_token_deltas(self, bus):
        """Turno LLM publica LlmTokenDelta para cada token."""
        tokens = ['{"reply":', '"oi"', ',"expression_id":0', ',"action":0,"emot_event":2}']
        llm = MockStreamingLLM(tokens=tokens)
        orch = _make_orch(bus, llm)
        asyncio.create_task(orch.run())
        await asyncio.sleep(0)

        collected, _ = await _run_llm_turn(bus, _good_transcript())
        deltas = [e for e in collected if isinstance(e, LlmTokenDelta)]
        assert len(deltas) == len(tokens)

    @pytest.mark.asyncio
    async def test_llm_turn_publishes_sentence_ready(self, bus):
        """Turno LLM publica SentenceReady para cada frase do reply."""
        llm = MockStreamingLLM(reply="Olá! Como posso ajudar você hoje?")
        orch = _make_orch(bus, llm)
        asyncio.create_task(orch.run())
        await asyncio.sleep(0)

        collected, _ = await _run_llm_turn(bus, _good_transcript())
        sentences = [e for e in collected if isinstance(e, SentenceReady)]
        assert len(sentences) >= 1

    @pytest.mark.asyncio
    async def test_llm_turn_fsm_returns_to_idle(self, bus):
        """Após turno LLM, FSM volta para IDLE."""
        from bridgev2.runtime.turn_manager import TurnState
        llm = MockStreamingLLM()
        orch = _make_orch(bus, llm)
        asyncio.create_task(orch.run())
        await asyncio.sleep(0)

        await _run_llm_turn(bus, _good_transcript())
        assert orch._fsm.is_idle

    @pytest.mark.asyncio
    async def test_llm_metrics_recorded(self, bus):
        """Métricas llm_first_token_ms e llm_total_ms são registradas."""
        llm = MockStreamingLLM()
        orch = _make_orch(bus, llm)
        asyncio.create_task(orch.run())
        await asyncio.sleep(0)

        await _run_llm_turn(bus, _good_transcript())
        assert orch.metrics.count("llm_first_token_ms") >= 1
        assert orch.metrics.count("llm_total_ms") >= 1

    @pytest.mark.asyncio
    async def test_llm_expression_id_propagated(self, bus):
        """expression_id do LLM chega no LlmReplyComplete."""
        llm = MockStreamingLLM(expression_id=3)
        orch = _make_orch(bus, llm)
        asyncio.create_task(orch.run())
        await asyncio.sleep(0)

        collected, _ = await _run_llm_turn(bus, _good_transcript())
        reply = next(e for e in collected if isinstance(e, LlmReplyComplete))
        assert reply.expression_id == 3

    @pytest.mark.asyncio
    async def test_llm_provider_name_in_reply(self, bus):
        """provider_name do mock aparece no LlmReplyComplete."""
        llm = MockStreamingLLM()
        orch = _make_orch(bus, llm)
        asyncio.create_task(orch.run())
        await asyncio.sleep(0)

        collected, _ = await _run_llm_turn(bus, _good_transcript())
        reply = next(e for e in collected if isinstance(e, LlmReplyComplete))
        assert reply.provider == "mock"


class TestLLMFailureHandling:
    @pytest.mark.asyncio
    async def test_llm_exception_emits_turn_error(self, bus):
        """Exceção no LLM → TurnError(stage='llm') publicado."""
        llm = MockStreamingLLM(raise_exc=RuntimeError("API offline"))
        orch = _make_orch(bus, llm)
        asyncio.create_task(orch.run())
        await asyncio.sleep(0)

        collected, final = await _run_llm_turn(bus, _good_transcript())
        assert isinstance(final, TurnError)
        assert final.stage == "llm"

    @pytest.mark.asyncio
    async def test_llm_exception_fsm_returns_to_idle(self, bus):
        """Após falha LLM, FSM volta para IDLE sem travar."""
        llm = MockStreamingLLM(raise_exc=ConnectionError("timeout"))
        orch = _make_orch(bus, llm)
        asyncio.create_task(orch.run())
        await asyncio.sleep(0)

        await _run_llm_turn(bus, _good_transcript())
        assert orch._fsm.is_idle

    @pytest.mark.asyncio
    async def test_circuit_open_error_emits_turn_error(self, bus):
        """CircuitOpenError → TurnError(stage='llm')."""
        llm = MockStreamingLLM(
            raise_exc=CircuitOpenError("openai/gpt-4o-mini", 25.0)
        )
        orch = _make_orch(bus, llm)
        asyncio.create_task(orch.run())
        await asyncio.sleep(0)

        _, final = await _run_llm_turn(bus, _good_transcript())
        assert isinstance(final, TurnError)
        assert final.stage == "llm"

    @pytest.mark.asyncio
    async def test_circuit_open_fsm_returns_to_idle(self, bus):
        """Circuit open + TurnError → IDLE (não trava)."""
        llm = MockStreamingLLM(
            raise_exc=CircuitOpenError("openai/gpt-4o-mini", 25.0)
        )
        orch = _make_orch(bus, llm)
        asyncio.create_task(orch.run())
        await asyncio.sleep(0)

        await _run_llm_turn(bus, _good_transcript())
        assert orch._fsm.is_idle

    @pytest.mark.asyncio
    async def test_multiple_turns_after_failure(self, bus):
        """Dois turnos: primeiro falha, segundo succeeds. Ambos terminam em IDLE."""
        fail_llm = MockStreamingLLM(raise_exc=RuntimeError("erro"))
        orch = _make_orch(bus, fail_llm)
        asyncio.create_task(orch.run())
        await asyncio.sleep(0)

        # Primeiro turno — falha
        await _run_llm_turn(bus, _good_transcript(turn_id=1))
        assert orch._fsm.is_idle

        # Troca provider para um que funciona
        orch._llm = MockStreamingLLM(reply="Agora funciona!")
        await _run_llm_turn(bus, _good_transcript(turn_id=2))
        assert orch._fsm.is_idle


class TestNoLLMProvider:
    @pytest.mark.asyncio
    async def test_no_llm_turns_idle(self, bus):
        """Sem LLM provider, turno sem intent local termina em IDLE."""
        orch = _make_orch(bus, llm=None)
        asyncio.create_task(orch.run())
        await asyncio.sleep(0)

        # Sem LLM: sem SpeechDone, sem TurnError — apenas retorna a IDLE
        ft = _good_transcript()
        await bus.publish(ft)
        await asyncio.sleep(0.1)
        assert orch._fsm.is_idle

    @pytest.mark.asyncio
    async def test_local_intent_still_works_without_llm(self, bus):
        """Intent local funciona independente do LLM provider."""
        orch = _make_orch(bus, llm=None)
        asyncio.create_task(orch.run())
        await asyncio.sleep(0)

        q: asyncio.Queue = asyncio.Queue()
        sub = bus.subscribe(maxsize=50)

        async def _drain():
            async for ev in EventBus.iter_queue(sub):
                if isinstance(ev, SpeechDone):
                    await q.put(ev)

        drain_task = asyncio.create_task(_drain())

        # "que horas são" → intent local local_time
        ft = FinalTranscript(
            turn_id=10,
            text="que horas são",
            quality=TranscriptQuality.GOOD,
        )
        await bus.publish(ft)
        done = await asyncio.wait_for(q.get(), timeout=2.0)
        drain_task.cancel()
        try:
            await drain_task
        except (asyncio.CancelledError, Exception):
            pass
        assert isinstance(done, SpeechDone)


class TestBatchLLMProvider:
    @pytest.mark.asyncio
    async def test_batch_style_provider_works(self, bus):
        """Provider que emite resposta inteira como único 'token' funciona."""
        llm = MockBatchLLM(reply="Resposta completa aqui disponível.")
        orch = _make_orch(bus, llm)
        asyncio.create_task(orch.run())
        await asyncio.sleep(0)

        collected, final = await _run_llm_turn(bus, _good_transcript())
        assert isinstance(final, SpeechDone)
        replies = [e for e in collected if isinstance(e, LlmReplyComplete)]
        assert "Resposta completa" in replies[0].reply
