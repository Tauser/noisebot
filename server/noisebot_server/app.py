"""Application composition for the local NoiseBot server."""

from __future__ import annotations

import asyncio
import logging
import os
from pathlib import Path
from typing import Any

from .config import LlmProvider, NoiseBotServerConfig, PipelineMode
from .internal.agent import (
    EventBus,
    DEFAULT_INITIAL_PROMPT,
    GeminiProvider,
    OllamaProvider,
    OpenAIStreamingProvider,
    Orchestrator,
    PiperServerTTS,
    WhisperLocalSTT,
)
from .internal.ops import OpsHttpServer, StatusStore
from .internal.ops.firmware_diag import FirmwareDiagClient
from .internal.service import healthcheck_loop
from .internal.transport import ConnectionSupervisor, create_transport_factory

log = logging.getLogger(__name__)


class NoiseBotServer:
    """Server-owned application graph.

    This class owns composition and lifecycle, so normal startup no longer
    depends on the legacy bridge application object.
    """

    def __init__(self, config: NoiseBotServerConfig) -> None:
        self._config = config
        self._bus = EventBus(default_maxsize=256)
        self._supervisor: Any | None = None
        self._tasks: list[asyncio.Task] = []
        self._running = False

        stt_provider = self._build_stt_provider()
        llm_provider = self._build_llm_provider()
        self._tts_provider = self._build_tts_provider()
        log.info(
            "TTS provider: %s",
            type(self._tts_provider).__name__
            if self._tts_provider is not None
            else "desabilitado",
        )

        self._status_store = StatusStore()
        self._orchestrator = Orchestrator(
            self._bus,
            config,
            get_adapter=self._get_adapter,
            stt_provider=stt_provider,
            llm_provider=llm_provider,
            tts_provider=self._tts_provider,
            status_store=self._status_store,
        )
        self._ops_server = self._build_ops_server()

        transport = config.transport
        if not config.dry_run and (transport.use_tcp or transport.uart):
            self._supervisor = self._build_supervisor()

    def _get_adapter(self) -> Any | None:
        if self._supervisor is None:
            return None
        return self._supervisor.adapter

    def _build_stt_provider(self) -> Any | None:
        stt = self._config.stt
        audio = self._config.audio
        if stt.backend != "faster":
            log.warning("STT backend %s ainda nao implementado; rodando sem STT.", stt.backend)
            return None
        return WhisperLocalSTT(
            model=stt.model,
            device=stt.device,
            compute_type=stt.compute_type,
            language=os.environ.get("NOISEBOT_WHISPER_LANGUAGE", "pt") or None,
            max_no_speech_prob=audio.max_no_speech_prob,
            min_avg_logprob=audio.min_avg_logprob,
            max_compression_ratio=audio.max_compression_ratio,
            min_rms=audio.min_transcribe_rms,
            min_peak=audio.min_transcribe_peak,
            beam_size=stt.beam_size,
            denoise_enabled=_env_bool("NOISEBOT_STT_DENOISE", False),
            vad_filter_enabled=_env_bool("NOISEBOT_WHISPER_VAD_FILTER", False),
            initial_prompt=os.environ.get("NOISEBOT_WHISPER_INITIAL_PROMPT", DEFAULT_INITIAL_PROMPT),
            noise_gate_mult=_env_float("NOISEBOT_STT_NOISE_GATE_MULT", 1.8),
            noise_gate_floor=_env_float("NOISEBOT_STT_NOISE_GATE_FLOOR", 0.003),
        )

    def _build_llm_provider(self) -> Any | None:
        llm = self._config.llm
        if self._config.pipeline_mode == PipelineMode.LOCAL_ONLY:
            return None
        if llm.provider == LlmProvider.NONE:
            return None
        if llm.provider == LlmProvider.OPENAI:
            return OpenAIStreamingProvider(
                model=llm.model,
                temperature=llm.temperature,
                max_tokens=llm.max_output_tokens,
            )
        if llm.provider == LlmProvider.GEMINI:
            return GeminiProvider(
                model=llm.model,
                temperature=llm.temperature,
                max_tokens=llm.max_output_tokens,
            )
        if llm.provider == LlmProvider.OLLAMA:
            return OllamaProvider(
                model=llm.model,
                base_url=llm.ollama_base_url,
                temperature=llm.temperature,
                max_tokens=llm.max_output_tokens,
                think=llm.ollama_think,
            )
        log.warning("LLM provider %s ainda nao implementado; rodando sem LLM.", llm.provider)
        return None

    def _build_tts_provider(self) -> Any | None:
        tts = self._config.tts
        if not tts.piper_model:
            log.warning("TTS: NOISEBOT_PIPER_MODEL nao configurado - TTS desabilitado.")
            return None
        log.info("TTS: Piper configurado modelo=%s", tts.piper_model)
        cache_dir = _tts_cache_dir()
        return PiperServerTTS(
            executable=tts.piper_executable,
            model=tts.piper_model,
            cache_size=tts.cache_size,
            disk_cache_dir=cache_dir,
            sample_rate=tts.sample_rate,
            target_peak=tts.target_peak,
        )

    def _build_ops_server(self) -> OpsHttpServer:
        return OpsHttpServer(
            app=self,
            store=self._status_store,
            host="127.0.0.1",
            port=self._config.ops.port,
        )

    def _build_supervisor(self) -> ConnectionSupervisor:
        return ConnectionSupervisor(
            transport_factory=create_transport_factory(self._config),
            bus=self._bus,
            reconnect=self._config.reconnect,
        )

    async def run(self) -> None:
        """Start the server runtime and block until shutdown."""
        self._running = True
        log.info(
            "NoiseBotServer.run() pipeline_mode=%s dry_run=%s",
            self._config.pipeline_mode.value,
            self._config.dry_run,
        )

        if self._tts_provider is not None:
            try:
                await self._tts_provider.initialize()
            except Exception:
                log.exception("TTS initialize falhou - TTS desabilitado para esta sessao.")
                self._tts_provider = None
                self._orchestrator.set_tts_provider(None)

        self._tasks.append(
            asyncio.create_task(self._orchestrator.run(), name="nb_orchestrator")
        )
        self._tasks.append(
            asyncio.create_task(healthcheck_loop(), name="nb_healthcheck")
        )

        try:
            await self._ops_server.start()
        except Exception:
            log.exception("Ops API: falha ao iniciar - pipeline continua sem dashboard.")

        await self._apply_default_audio_codec()

        if self._supervisor is not None:
            self._tasks.append(
                asyncio.create_task(self._supervisor.run(), name="nb_supervisor")
            )
            log.info(
                "NoiseBot server ativo com transporte %s. Aguardando firmware...",
                self._config.transport.host or self._config.transport.uart,
            )
        else:
            log.info("NoiseBot server ativo sem transporte. Aguardando eventos.")

        await asyncio.gather(*self._tasks, return_exceptions=True)

    async def shutdown(self) -> None:
        """Stop all server components gracefully."""
        if not self._running:
            return
        self._running = False
        log.info("NoiseBotServer: encerrando...")

        await self._ops_server.stop()

        if self._supervisor is not None:
            await self._supervisor.shutdown()

        await self._orchestrator.shutdown()

        for task in self._tasks:
            if not task.done():
                task.cancel()
        if self._tasks:
            await asyncio.gather(*self._tasks, return_exceptions=True)
        self._tasks.clear()
        log.info("NoiseBotServer: encerrado.")

    async def _apply_default_audio_codec(self) -> None:
        codec = self._config.audio.default_codec
        if codec == "pcm16":
            log.info("Audio codec default: pcm16")
            return

        if codec != "opus-v2":
            log.warning("Audio codec default desconhecido: %s", codec)
            return

        client = FirmwareDiagClient.from_config(self._config)
        if client is None:
            log.warning("Audio codec default opus-v2 ignorado: firmware HTTP indisponivel")
            return

        try:
            payload = await asyncio.to_thread(client.audio_codec_v2_transport_enable)
        except Exception:
            log.exception("Audio codec default opus-v2 falhou ao habilitar transporte")
            return

        if not payload.get("ok") or not payload.get("opus_enabled"):
            log.warning("Audio codec default opus-v2 nao confirmou enable: %s", payload)
            return

        log.info("Audio codec default opus-v2 habilitado via Codec v2")


def _tts_cache_dir() -> Path | None:
    configured = os.environ.get("NOISEBOT_TTS_CACHE_DIR", "").strip()
    if configured.lower() in {"0", "false", "off", "none"}:
        return None
    if configured:
        return Path(configured)
    return Path.home() / ".noisebot-server" / "tts-cache"


def _env_bool(key: str, default: bool) -> bool:
    raw = os.environ.get(key)
    if raw is None:
        return default
    return raw.strip().lower() not in {"0", "false", "off", "no", "nao", "não"}


def _env_float(key: str, default: float) -> float:
    try:
        return float(os.environ.get(key, default))
    except (TypeError, ValueError):
        return default
