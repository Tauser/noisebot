"""Testes de integração — Fase 4: STT mock → Orchestrator → FinalTranscript → intent.

Usa um STTProvider mock para verificar:
  - VoiceActivityEnd com áudio suficiente → STT worker lançado → FinalTranscript publicado
  - FinalTranscript do STT → intent local → RobotCommand no bus
  - Métricas de latência registradas (stt_ms, end_of_turn_ms, etc.)
  - STT None → comportamento Fase 3 (injeção sintética continua funcionando)
  - VoiceActivityEnd com áudio curto → descartado (sem STT)
  - TurnError publicado se STT lança exceção inesperada

Design:
  - STTProvider mock tem finalize() controlável por asyncio.Event
  - AudioChunkIn com PCM real (amplitude ≥ min_rms) para passar rejeição
  - _wait_event / _drain helpers da suite Fase 3
"""
from __future__ import annotations

import asyncio
import struct
from unittest.mock import AsyncMock, MagicMock

import pytest

from bridgev2.config import (
    AudioConfig, BridgeV2Config, LlmConfig, LlmProvider, LogLevel,
    OpsConfig, PipelineMode, ReconnectConfig, SttConfig, TransportConfig,
    TtsConfig,
)
from bridgev2.runtime.bus import EventBus
from bridgev2.runtime.events import (
    AudioChunkIn,
    FinalTranscript,
    IntentResolved,
    RobotCommand,
    SpeechDone,
    TurnError,
    TranscriptQuality,
    VoiceActivityEnd,
    VoiceActivityStart,
    WakeDetected,
)
from bridgev2.runtime.orchestrator import Orchestrator
from bridgev2.runtime.turn_manager import TurnState
from bridgev2.stt.base import STTProvider


# ── Fixtures ──────────────────────────────────────────────────────────────────

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


def _loud_pcm(n_samples: int = 256, amplitude: int = 5000) -> bytes:
    """PCM int16 LE com amplitude alta (passa rejeição de RMS)."""
    half = n_samples // 2
    samples = [amplitude] * half + [-amplitude] * (n_samples - half)
    return struct.pack(f"<{n_samples}h", *samples)


class MockSTT(STTProvider):
    """STT mock controlável: finalize() retorna o FinalTranscript configurado."""

    def __init__(self, result: FinalTranscript | None = None, raise_on_finalize: bool = False):
        self._result = result
        self._raise = raise_on_finalize
        self.feed_calls: list[bytes] = []
        self.finalize_calls: int = 0
        self.reset_calls: int = 0
        self.initialized: bool = False
        self.closed: bool = False

    async def initialize(self) -> None:
        self.initialized = True

    def feed(self, pcm: bytes) -> None:
        self.feed_calls.append(pcm)

    async def partial(self, turn_id: int):
        from bridgev2.runtime.events import PartialTranscript
        return PartialTranscript(turn_id=turn_id, text="")

    async def finalize(self, full_pcm: bytes, turn_id: int) -> FinalTranscript:
        self.finalize_calls += 1
        if self._raise:
            raise RuntimeError("STT mock error")
        if self._result is not None:
            # Usa o turn_id real do turno ativo, não o do template de result
            return FinalTranscript(
                turn_id=turn_id,
                text=self._result.text,
                quality=self._result.quality,
            )
        return FinalTranscript(turn_id=turn_id, text="tudo bem", quality=TranscriptQuality.GOOD)

    async def reset(self) -> None:
        self.reset_calls += 1

    async def close(self) -> None:
        self.closed = True


async def _wait_event(q: asyncio.Queue, timeout: float = 3.0):
    return await asyncio.wait_for(q.get(), timeout=timeout)


async def _drain(q: asyncio.Queue, timeout: float = 0.3) -> list:
    items = []
    try:
        while True:
            items.append(await asyncio.wait_for(q.get(), timeout=timeout))
    except asyncio.TimeoutError:
        pass
    return items


async def _simulate_voice_session(
    bus: EventBus,
    text: str = "tudo bem",
    n_chunks: int = 40,   # 40 × 256 amostras = 10240 > 8000 mínimo
    amplitude: int = 5000,
) -> None:
    """Simula: WakeDetected → AudioChunks → VoiceActivityEnd."""
    await bus.publish(WakeDetected())
    await asyncio.sleep(0)  # yield para o orchestrator processar

    pcm = _loud_pcm(n_samples=256, amplitude=amplitude)
    for _ in range(n_chunks):
        await bus.publish(AudioChunkIn(pcm=pcm, seq=0))

    await asyncio.sleep(0)  # yield
    await bus.publish(VoiceActivityEnd())


# ── Fluxo completo com STT mock ───────────────────────────────────────────────

class TestSttWorkerFlow:
    async def test_voice_session_produces_final_transcript(self):
        """VoiceActivityEnd com áudio suficiente → STT → FinalTranscript no bus."""
        bus = EventBus(default_maxsize=512)
        stt = MockSTT()
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None, stt_provider=stt)
        q_ft = bus.subscribe(FinalTranscript)
        q_done = bus.subscribe(SpeechDone)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await _simulate_voice_session(bus)
            ft = await _wait_event(q_ft, timeout=3.0)
            assert isinstance(ft, FinalTranscript)
            assert ft.text == "tudo bem"

            # Aguarda o turno completar
            await _wait_event(q_done, timeout=3.0)
            assert orch._fsm.state == TurnState.IDLE
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)

    async def test_stt_finalize_called_once_per_turn(self):
        """STT.finalize() é chamado exatamente uma vez por turno."""
        bus = EventBus(default_maxsize=512)
        stt = MockSTT()
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None, stt_provider=stt)
        q_done = bus.subscribe(SpeechDone)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await _simulate_voice_session(bus)
            await _wait_event(q_done, timeout=3.0)
            assert stt.finalize_calls == 1
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)

    async def test_stt_feed_called_for_each_audio_chunk(self):
        """STT.feed() é chamado para cada AudioChunkIn no estado LISTENING."""
        bus = EventBus(default_maxsize=512)
        stt = MockSTT()
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None, stt_provider=stt)
        q_done = bus.subscribe(SpeechDone)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await _simulate_voice_session(bus, n_chunks=40)
            await _wait_event(q_done, timeout=3.0)
            assert stt.feed_calls  # pelo menos alguns chunks foram alimentados
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)

    async def test_intent_resolved_after_stt(self):
        """Após STT, intent é resolvido e publicado no bus."""
        bus = EventBus(default_maxsize=512)
        stt = MockSTT(result=FinalTranscript(turn_id=0, text="olá", quality=TranscriptQuality.GOOD))
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None, stt_provider=stt)
        q_intent = bus.subscribe(IntentResolved)
        q_done = bus.subscribe(SpeechDone)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await _simulate_voice_session(bus)
            await _wait_event(q_done, timeout=3.0)

            intent = await _wait_event(q_intent, timeout=1.0)
            assert intent.intent_name == "local_greeting"
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)

    async def test_robot_command_emitted_after_stt(self):
        """RobotCommand é publicado no bus após o fluxo STT → intent."""
        bus = EventBus(default_maxsize=512)
        stt = MockSTT()
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None, stt_provider=stt)
        q_cmd = bus.subscribe(RobotCommand, maxsize=64)
        q_done = bus.subscribe(SpeechDone)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await _simulate_voice_session(bus)
            await _wait_event(q_done, timeout=3.0)

            cmds = await _drain(q_cmd)
            assert cmds, "Esperado ao menos um RobotCommand"
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)


# ── Métricas de latência ──────────────────────────────────────────────────────

class TestMetrics:
    async def test_stt_ms_recorded(self):
        """Após turno com STT, stt_ms é registrado nas métricas."""
        bus = EventBus(default_maxsize=512)
        stt = MockSTT()
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None, stt_provider=stt)
        q_done = bus.subscribe(SpeechDone)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await _simulate_voice_session(bus)
            await _wait_event(q_done, timeout=3.0)

            assert orch.metrics.count("stt_ms") == 1
            stt_ms = orch.metrics.p50("stt_ms")
            assert stt_ms is not None
            assert stt_ms >= 0  # deve ser positivo
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)

    async def test_end_of_turn_ms_recorded(self):
        """end_of_turn_ms registrado após turno com STT."""
        bus = EventBus(default_maxsize=512)
        stt = MockSTT()
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None, stt_provider=stt)
        q_done = bus.subscribe(SpeechDone)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await _simulate_voice_session(bus)
            await _wait_event(q_done, timeout=3.0)

            assert orch.metrics.count("end_of_turn_ms") == 1
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)

    async def test_local_intent_ms_recorded(self):
        """local_intent_ms registrado após resolução de intent."""
        bus = EventBus(default_maxsize=512)
        stt = MockSTT()
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None, stt_provider=stt)
        q_done = bus.subscribe(SpeechDone)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await _simulate_voice_session(bus)
            await _wait_event(q_done, timeout=3.0)

            assert orch.metrics.count("local_intent_ms") >= 1
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)


# ── Áudio curto (descartado) ──────────────────────────────────────────────────

class TestShortAudio:
    async def test_short_audio_not_sent_to_stt(self):
        """VoiceActivityEnd com < 8000 amostras → descartado sem chamar STT."""
        bus = EventBus(default_maxsize=512)
        stt = MockSTT()
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None, stt_provider=stt)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await bus.publish(WakeDetected())
            await asyncio.sleep(0)

            # Apenas 10 chunks × 256 = 2560 amostras < 8000
            pcm = _loud_pcm(256, 5000)
            for _ in range(10):
                await bus.publish(AudioChunkIn(pcm=pcm, seq=0))

            await asyncio.sleep(0)
            await bus.publish(VoiceActivityEnd())
            await asyncio.sleep(0.1)  # deixa o orchestrator processar

            assert stt.finalize_calls == 0
            assert orch._fsm.state == TurnState.IDLE
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)


# ── Erro no STT → TurnError ───────────────────────────────────────────────────

class TestSttError:
    async def test_stt_exception_publishes_turn_error(self):
        """Exceção no STT → TurnError publicado no bus → FSM → IDLE."""
        bus = EventBus(default_maxsize=512)
        stt = MockSTT(raise_on_finalize=True)
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None, stt_provider=stt)
        q_err = bus.subscribe(TurnError)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await _simulate_voice_session(bus)
            err = await _wait_event(q_err, timeout=3.0)
            assert isinstance(err, TurnError)
            assert err.stage == "stt"

            # Orchestrator deve voltar a IDLE após o erro
            await asyncio.sleep(0.1)
            assert orch._fsm.state == TurnState.IDLE
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)


# ── STT=None (Fase 3 compat) ─────────────────────────────────────────────────

class TestNoSttProvider:
    async def test_synthetic_injection_still_works_without_stt(self):
        """Com STT=None, injeção sintética de FinalTranscript continua funcionando."""
        bus = EventBus(default_maxsize=512)
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None, stt_provider=None)
        q_intent = bus.subscribe(IntentResolved)
        q_done = bus.subscribe(SpeechDone)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            await bus.publish(FinalTranscript(turn_id=1, text="olá"))
            await _wait_event(q_done, timeout=2.0)

            intent = await _wait_event(q_intent, timeout=1.0)
            assert intent.intent_name == "local_greeting"
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)


# ── Múltiplos turnos sequenciais com STT ─────────────────────────────────────

class TestMultipleTurnsWithStt:
    async def test_two_turns_stt_metrics_accumulate(self):
        """Dois turnos com STT: count de stt_ms == 2."""
        bus = EventBus(default_maxsize=512)
        stt = MockSTT()
        orch = Orchestrator(bus, _make_config(), get_adapter=lambda: None, stt_provider=stt)
        q_done = bus.subscribe(SpeechDone, maxsize=64)

        orch_task = asyncio.create_task(orch.run(), name="orch")
        try:
            # Turno 1
            await _simulate_voice_session(bus)
            await _wait_event(q_done, timeout=3.0)

            # Turno 2
            await _simulate_voice_session(bus)
            await _wait_event(q_done, timeout=3.0)

            assert stt.finalize_calls == 2
            assert orch.metrics.count("stt_ms") == 2
        finally:
            await orch.shutdown()
            orch_task.cancel()
            await asyncio.gather(orch_task, return_exceptions=True)
