"""Testes de integracao: Application -- caminho real Application -> ConnectionSupervisor.

Cobre os pontos levantados na revisao:
  - dry_run=True nao cria supervisor mesmo com host configurado
  - sem host/uart nao cria supervisor
  - caminho TCP real: Application -> supervisor -> adapter -> FakeFirmware
  - get_adapter() retorna adapter ativo apos conexao, None antes
"""
from __future__ import annotations

import asyncio
import pytest

from bridgev2.app import Application
from bridgev2.config import (
    BridgeV2Config, TransportConfig, LlmConfig, LlmProvider,
    PipelineMode, SttConfig, TtsConfig, AudioConfig,
    ReconnectConfig, OpsConfig, LogLevel,
)
from bridgev2.debug.fake_firmware import FakeFirmware
from bridgev2.runtime.events import FirmwareConnected, FirmwareDisconnected

_PORT_BASE = 19200


def _port(offset: int) -> int:
    return _PORT_BASE + offset


def _make_config(
    *,
    host: str | None = None,
    port: int = 9000,
    uart: str | None = None,
    dry_run: bool = False,
    reconnect_delay: float = 0.05,
    piper_model: str = "",
) -> BridgeV2Config:
    """Constroi BridgeV2Config minimo para testes."""
    return BridgeV2Config(
        transport=TransportConfig(
            host=host,
            port=port,
            uart=uart,
            baudrate=1000000,
        ),
        llm=LlmConfig(
            provider=LlmProvider.NONE,
            model="none",
            timeout_s=5.0,
            max_output_tokens=140,
            max_reply_chars=180,
            openai_key_configured=False,
            gemini_key_configured=False,
        ),
        pipeline_mode=PipelineMode.LOCAL_ONLY,
        stt=SttConfig(model="small", backend="faster", device="cpu", compute_type="int8"),
        tts=TtsConfig(
            piper_executable="piper", piper_model=piper_model,
            cache_size=4, sample_rate=16000, target_peak=8000,
        ),
        audio=AudioConfig(
            chunk_samples=256, sample_rate=16000,
            min_transcribe_rms=140.0, min_transcribe_peak=1600,
            min_utterance_samples=8000,
            max_no_speech_prob=0.75, min_avg_logprob=-1.10,
            max_compression_ratio=2.60,
        ),
        reconnect=ReconnectConfig(
            delay_s=reconnect_delay,
            max_delay_s=0.2,
            connect_timeout_s=2.0,
        ),
        ops=OpsConfig(port=8765, token_configured=False),
        log_level=LogLevel.DEBUG,
        dry_run=dry_run,
        replay_path=None,
    )


# ── dry_run e configuracoes sem transporte ────────────────────────────────────

class TestApplicationInit:
    def test_dry_run_suppresses_supervisor(self):
        """dry_run=True nao cria supervisor mesmo com host configurado."""
        config = _make_config(host="127.0.0.1", port=9000, dry_run=True)
        app = Application(config)
        assert app._supervisor is None

    def test_no_transport_no_supervisor(self):
        """Sem host nem uart, nao cria supervisor."""
        config = _make_config()
        app = Application(config)
        assert app._supervisor is None

    def test_tcp_config_creates_supervisor(self):
        """Com host configurado e dry_run=False, supervisor e criado."""
        config = _make_config(host="127.0.0.1", port=9999)
        app = Application(config)
        assert app._supervisor is not None

    def test_get_adapter_returns_none_before_connect(self):
        """get_adapter() retorna None antes de qualquer conexao."""
        config = _make_config(host="127.0.0.1", port=9999)
        app = Application(config)
        assert app._get_adapter() is None

    async def test_tts_init_failure_disables_orchestrator_provider(self, monkeypatch):
        """Falha no Piper no boot não deixa provider quebrado preso no Orchestrator."""
        class BadTTS:
            async def initialize(self):
                raise RuntimeError("piper indisponivel")

            async def shutdown(self):
                pass

        monkeypatch.setattr(
            "bridgev2.tts.piper_server.PiperServerTTS",
            lambda **kwargs: BadTTS(),
        )

        config = _make_config(dry_run=True, piper_model="fake.onnx")
        app = Application(config)
        task = asyncio.create_task(app.run(), name="test_app_bad_tts")
        try:
            await asyncio.sleep(0.05)
            assert app._tts_provider is None
            assert app._orchestrator._tts is None
        finally:
            await app.shutdown()
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)


# ── Caminho real Application -> supervisor -> FakeFirmware ────────────────────

class TestApplicationTcpIntegration:
    async def test_firmware_connected_via_application(self):
        """Application TCP conecta ao FakeFirmware e publica FirmwareConnected no bus."""
        port = _port(1)
        config = _make_config(host="127.0.0.1", port=port, reconnect_delay=0.05)
        app = Application(config)

        q = app._bus.subscribe(FirmwareConnected)

        fw = FakeFirmware(port=port)
        app_task = None
        try:
            await fw.start()
            app_task = asyncio.create_task(app.run(), name="test_app_run")

            event = await asyncio.wait_for(q.get(), timeout=4.0)
            assert isinstance(event, FirmwareConnected)
            assert isinstance(event.peer_capabilities, dict)
        finally:
            await app.shutdown()
            if app_task is not None:
                app_task.cancel()
                await asyncio.gather(app_task, return_exceptions=True)
            await fw.stop()

    async def test_get_adapter_returns_adapter_after_connect(self):
        """get_adapter() retorna adapter ativo apos FirmwareConnected."""
        port = _port(2)
        config = _make_config(host="127.0.0.1", port=port, reconnect_delay=0.05)
        app = Application(config)

        q = app._bus.subscribe(FirmwareConnected)

        fw = FakeFirmware(port=port)
        app_task = None
        try:
            await fw.start()
            app_task = asyncio.create_task(app.run(), name="test_app_adapter")

            await asyncio.wait_for(q.get(), timeout=4.0)
            # Apos FirmwareConnected, get_adapter() deve retornar o adapter ativo
            adapter = app._get_adapter()
            assert adapter is not None
        finally:
            await app.shutdown()
            if app_task is not None:
                app_task.cancel()
                await asyncio.gather(app_task, return_exceptions=True)
            await fw.stop()

    async def test_firmware_disconnected_via_application(self):
        """Quando firmware desconecta, FirmwareDisconnected chega no bus via Application."""
        port = _port(3)
        config = _make_config(host="127.0.0.1", port=port, reconnect_delay=0.05)
        app = Application(config)

        q_conn = app._bus.subscribe(FirmwareConnected)
        q_disc = app._bus.subscribe(FirmwareDisconnected)

        fw = FakeFirmware(port=port)
        app_task = None
        try:
            await fw.start()
            app_task = asyncio.create_task(app.run(), name="test_app_disc")

            await asyncio.wait_for(q_conn.get(), timeout=4.0)
            await fw.stop()

            event = await asyncio.wait_for(q_disc.get(), timeout=3.0)
            assert isinstance(event, FirmwareDisconnected)
        finally:
            await app.shutdown()
            if app_task is not None:
                app_task.cancel()
                await asyncio.gather(app_task, return_exceptions=True)
