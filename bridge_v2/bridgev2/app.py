"""bridgev2.app -- Composicao da aplicacao: cria providers, transporte e runtime.

Application e o ponto de montagem do grafo de dependencias.
Cada fase plugara um provider concreto aqui.
"""
from __future__ import annotations

import asyncio
import logging
from typing import Callable

from .config import BridgeV2Config, LlmProvider, PipelineMode
from .llm.gemini_provider import GeminiProvider
from .llm.openai_provider import OpenAIStreamingProvider
from .runtime.bus import EventBus
from .runtime.orchestrator import Orchestrator
from .stt.whisper_local import WhisperLocalSTT

log = logging.getLogger(__name__)


class Application:
    """Composicao do bridge v2.

    Fase 1: sobe o event loop, cria bus e orchestrator, nao conecta nada.
    Fase 2: se host ou uart configurados e nao dry_run, sobe ConnectionSupervisor.
    Cada fase subsequente adiciona providers reais.
    """

    def __init__(self, config: BridgeV2Config) -> None:
        self._config = config
        self._bus = EventBus(default_maxsize=256)
        self._supervisor = None
        self._tasks: list[asyncio.Task] = []
        self._running = False

        # get_adapter: callable que retorna o FirmwareAdapter ativo (ou None)
        # injetado no Orchestrator para que ele possa enviar comandos ao firmware
        get_adapter = self._get_adapter
        stt_provider = self._build_stt_provider()
        llm_provider = self._build_llm_provider()

        self._orchestrator = Orchestrator(
            self._bus,
            config,
            get_adapter=get_adapter,
            stt_provider=stt_provider,
            llm_provider=llm_provider,
        )

        # Fase 2: criar supervisor se transporte configurado e nao for dry_run
        transport = config.transport
        if not config.dry_run and (transport.use_tcp or transport.uart):
            self._supervisor = self._build_supervisor()

    def _get_adapter(self):
        """Retorna o FirmwareAdapter ativo ou None."""
        if self._supervisor is None:
            return None
        return self._supervisor.adapter

    def _build_stt_provider(self):
        """Cria o provider STT local. O modelo carrega lazy no primeiro turno."""
        stt = self._config.stt
        audio = self._config.audio
        if stt.backend != "faster":
            log.warning("STT backend %s ainda não implementado; rodando sem STT.", stt.backend)
            return None
        return WhisperLocalSTT(
            model=stt.model,
            device=stt.device,
            compute_type=stt.compute_type,
            max_no_speech_prob=audio.max_no_speech_prob,
            min_avg_logprob=audio.min_avg_logprob,
            max_compression_ratio=audio.max_compression_ratio,
            min_rms=audio.min_transcribe_rms,
        )

    def _build_llm_provider(self):
        """Cria o provider LLM remoto quando o modo permite."""
        llm = self._config.llm
        if self._config.pipeline_mode == PipelineMode.LOCAL_ONLY:
            return None
        if llm.provider == LlmProvider.NONE:
            return None
        if llm.provider == LlmProvider.OPENAI:
            return OpenAIStreamingProvider(
                model=llm.model,
                max_tokens=llm.max_output_tokens,
            )
        if llm.provider == LlmProvider.GEMINI:
            return GeminiProvider(
                model=llm.model,
                max_tokens=llm.max_output_tokens,
            )
        log.warning("LLM provider %s ainda não implementado; rodando sem LLM.", llm.provider)
        return None

    def _build_supervisor(self):
        """Constroi ConnectionSupervisor com a factory de transporte correta."""
        from .transport.reconnect import ConnectionSupervisor

        transport = self._config.transport
        reconnect = self._config.reconnect

        if transport.use_tcp:
            from .transport.tcp import TcpTransport

            def tcp_factory():
                return TcpTransport(
                    host=transport.host,
                    port=transport.port,
                    connect_timeout=reconnect.connect_timeout_s,
                )

            factory = tcp_factory
        else:
            from .transport.uart import UartTransport

            def uart_factory():
                return UartTransport(
                    port=transport.uart,
                    baudrate=transport.baudrate,  # TransportConfig.baudrate
                )

            factory = uart_factory

        return ConnectionSupervisor(
            transport_factory=factory,
            bus=self._bus,
            reconnect=reconnect,
        )

    async def run(self) -> None:
        """Inicia o event loop e todos os componentes."""
        self._running = True
        log.info(
            "Application.run() pipeline_mode=%s dry_run=%s",
            self._config.pipeline_mode.value,
            self._config.dry_run,
        )

        orch_task = asyncio.create_task(
            self._orchestrator.run(),
            name="nb_orchestrator",
        )
        self._tasks.append(orch_task)

        if self._supervisor is not None:
            supervisor_task = asyncio.create_task(
                self._supervisor.run(),
                name="nb_supervisor",
            )
            self._tasks.append(supervisor_task)
            log.info(
                "Bridge v2 ativo com transporte %s. Aguardando firmware...",
                self._config.transport.host or self._config.transport.uart,
            )
        else:
            log.info("Bridge v2 ativo (sem transporte). Aguardando eventos... (Ctrl+C para encerrar)")

        await asyncio.gather(*self._tasks, return_exceptions=True)

    async def shutdown(self) -> None:
        """Encerra todos os componentes graciosamente."""
        if not self._running:
            return
        self._running = False
        log.info("Application: encerrando...")

        if self._supervisor is not None:
            await self._supervisor.shutdown()

        await self._orchestrator.shutdown()

        for task in self._tasks:
            if not task.done():
                task.cancel()
        if self._tasks:
            await asyncio.gather(*self._tasks, return_exceptions=True)
        self._tasks.clear()
        log.info("Application: encerrado.")
