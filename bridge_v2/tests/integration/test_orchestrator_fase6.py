"""Testes de integracao para o caminho TTS/SAY (Fase 6)."""
from __future__ import annotations

from typing import AsyncIterator
from unittest.mock import AsyncMock, MagicMock

import asyncio
import pytest

from bridgev2.runtime.bus import EventBus
from bridgev2.runtime.events import FinalTranscript, SpeechDone, TranscriptQuality
from bridgev2.runtime.orchestrator import Orchestrator


class MockTTS:
    def __init__(self) -> None:
        self.sentences: list[str] = []

    async def initialize(self) -> None:
        pass

    async def shutdown(self) -> None:
        pass

    async def synthesize_stream(self, sentences: AsyncIterator[str]) -> AsyncIterator[bytes]:
        async for sentence in sentences:
            self.sentences.append(sentence)
            yield b"\x01\x02" * 256
            yield b"\x03\x04" * 256


class MockLLM:
    _provider_name = "mock"
    _model = "mock-model"

    async def _stream(self) -> AsyncIterator[str]:
        raw = '{"reply":"Oi! Tudo certo.","expression_id":1,"action":0,"emot_event":2}'
        for i in range(0, len(raw), 12):
            yield raw[i:i + 12]

    def generate_stream(self, text: str, context: dict) -> AsyncIterator[str]:
        return self._stream()


def _mock_adapter():
    adapter = MagicMock()
    adapter.send_say = AsyncMock()
    adapter.send_expr = AsyncMock()
    adapter.send_emot_event = AsyncMock()
    adapter.send_action = AsyncMock()
    adapter.send_text_scroll = AsyncMock()
    adapter.send_gaze = AsyncMock()
    return adapter


async def _wait_speech_done(bus: EventBus, timeout: float = 3.0) -> SpeechDone:
    q = bus.subscribe(SpeechDone)
    return await asyncio.wait_for(q.get(), timeout=timeout)


@pytest.mark.asyncio
async def test_local_intent_uses_tts_and_sends_say_chunks():
    bus = EventBus()
    tts = MockTTS()
    adapter = _mock_adapter()
    orch = Orchestrator(bus, get_adapter=lambda: adapter, tts_provider=tts)
    task = asyncio.create_task(orch.run())

    try:
        done_task = asyncio.create_task(_wait_speech_done(bus))
        await bus.publish(FinalTranscript(
            turn_id=10,
            text="que horas sao",
            quality=TranscriptQuality.GOOD,
        ))

        done = await done_task
        assert done.turn_id == 10
        assert tts.sentences
        assert adapter.send_say.call_count == len(tts.sentences) * 2
        assert orch.metrics.count("tts_first_audio_ms") == 1
        assert orch.metrics.count("first_audio_out_ms") == 1
    finally:
        await orch.shutdown()
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)


@pytest.mark.asyncio
async def test_llm_reply_uses_tts_and_sends_say_chunks():
    bus = EventBus()
    tts = MockTTS()
    adapter = _mock_adapter()
    orch = Orchestrator(
        bus,
        get_adapter=lambda: adapter,
        llm_provider=MockLLM(),
        tts_provider=tts,
    )
    task = asyncio.create_task(orch.run())

    try:
        done_task = asyncio.create_task(_wait_speech_done(bus))
        await bus.publish(FinalTranscript(
            turn_id=11,
            text="me conta algo",
            quality=TranscriptQuality.GOOD,
        ))

        done = await done_task
        assert done.turn_id == 11
        assert adapter.send_say.call_count == len(tts.sentences) * 2
        assert tts.sentences == ["Oi! Tudo certo."]
    finally:
        await orch.shutdown()
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)
