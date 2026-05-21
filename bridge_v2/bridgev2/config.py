"""bridgev2.config -- Configuracao tipada carregada de variaveis de ambiente / .env.

Regras de seguranca:
- Segredos (API keys) NUNCA aparecem no objeto BridgeV2Config persistido.
- Eles sao lidos diretamente de os.environ em tempo de execucao pelos providers.
- Este modulo NAO loga segredos; apenas confirma se estao configurados (bool).
"""
from __future__ import annotations

import logging
import os
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path

log = logging.getLogger(__name__)


# -- Enums ------------------------------------------------------------------


class LlmProvider(str, Enum):
    OPENAI = "openai"
    GEMINI = "gemini"
    NONE = "none"


class PipelineMode(str, Enum):
    """Modo de operacao do pipeline de voz."""
    NORMAL = "normal"         # STT + LLM streaming + TTS
    LOCAL_ONLY = "local_only" # apenas LocalIntentProvider; sem LLM remoto
    DEGRADED = "degraded"     # intents locais + LLM batch (sem streaming)
    REALTIME = "realtime"     # RealtimeProvider audio->audio (Fase 8, futuro)


class LogLevel(str, Enum):
    DEBUG = "DEBUG"
    INFO = "INFO"
    WARNING = "WARNING"
    ERROR = "ERROR"


# -- Config dataclasses -----------------------------------------------------


@dataclass(frozen=True)
class TransportConfig:
    host: str | None
    port: int
    uart: str | None
    baudrate: int = 1000000

    @property
    def use_tcp(self) -> bool:
        return bool(self.host)


@dataclass(frozen=True)
class LlmConfig:
    provider: LlmProvider
    model: str
    timeout_s: float
    max_output_tokens: int
    max_reply_chars: int
    # api_key_configured: apenas indica presenca -- nunca o valor
    openai_key_configured: bool
    gemini_key_configured: bool


@dataclass(frozen=True)
class SttConfig:
    model: str
    backend: str   # "faster" | "openai"
    device: str    # "cpu" | "cuda"
    compute_type: str  # "int8" | "float16" | ...


@dataclass(frozen=True)
class TtsConfig:
    piper_executable: str
    piper_model: str
    cache_size: int
    sample_rate: int
    target_peak: int


@dataclass(frozen=True)
class AudioConfig:
    chunk_samples: int
    sample_rate: int
    min_transcribe_rms: float
    min_transcribe_peak: int
    min_utterance_samples: int
    max_no_speech_prob: float
    min_avg_logprob: float
    max_compression_ratio: float


@dataclass(frozen=True)
class ReconnectConfig:
    delay_s: float
    max_delay_s: float
    connect_timeout_s: float = 5.0


@dataclass(frozen=True)
class OpsConfig:
    port: int
    token_configured: bool  # indica presenca do token -- nunca o valor


@dataclass(frozen=True)
class BridgeV2Config:
    """Configuracao completa do Bridge v2 -- sem segredos."""
    transport: TransportConfig
    llm: LlmConfig
    pipeline_mode: PipelineMode
    stt: SttConfig
    tts: TtsConfig
    audio: AudioConfig
    reconnect: ReconnectConfig
    ops: OpsConfig
    log_level: LogLevel
    dry_run: bool
    replay_path: str | None

    def safe_dict(self) -> dict:
        """Retorna representacao sem segredos -- seguro para logs e API de operacao."""
        return {
            "transport": {
                "host": self.transport.host,
                "port": self.transport.port,
                "uart": self.transport.uart,
                "baudrate": self.transport.baudrate,
                "use_tcp": self.transport.use_tcp,
            },
            "llm": {
                "provider": self.llm.provider.value,
                "model": self.llm.model,
                "timeout_s": self.llm.timeout_s,
                "max_output_tokens": self.llm.max_output_tokens,
                "openai_key_configured": self.llm.openai_key_configured,
                "gemini_key_configured": self.llm.gemini_key_configured,
            },
            "pipeline_mode": self.pipeline_mode.value,
            "stt": {
                "model": self.stt.model,
                "backend": self.stt.backend,
                "device": self.stt.device,
                "compute_type": self.stt.compute_type,
            },
            "tts": {
                "piper_executable": self.tts.piper_executable,
                "piper_model": self.tts.piper_model,
                "cache_size": self.tts.cache_size,
            },
            "audio": {
                "chunk_samples": self.audio.chunk_samples,
                "sample_rate": self.audio.sample_rate,
                "min_transcribe_rms": self.audio.min_transcribe_rms,
                "min_transcribe_peak": self.audio.min_transcribe_peak,
            },
            "reconnect": {
                "delay_s": self.reconnect.delay_s,
                "max_delay_s": self.reconnect.max_delay_s,
                "connect_timeout_s": self.reconnect.connect_timeout_s,
            },
            "ops": {
                "port": self.ops.port,
                "token_configured": self.ops.token_configured,
            },
            "log_level": self.log_level.value,
            "dry_run": self.dry_run,
        }


# -- Carregamento de .env ---------------------------------------------------


def load_env_file(path: str | os.PathLike | None = None) -> None:
    """Le arquivo .env e injeta em os.environ (sem sobrescrever variaveis existentes)."""
    env_path = (
        Path(path)
        if path is not None
        else Path(__file__).resolve().parents[1] / ".env"
    )
    if not env_path.exists():
        return

    for raw_line in env_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        if key and key not in os.environ:
            os.environ[key] = value


def _env(key: str, default: str = "") -> str:
    return os.environ.get(key, default)


def _env_int(key: str, default: int) -> int:
    try:
        return int(os.environ.get(key, default))
    except (ValueError, TypeError):
        return default


def _env_float(key: str, default: float) -> float:
    try:
        return float(os.environ.get(key, default))
    except (ValueError, TypeError):
        return default


def _env_bool(key: str, default: bool = False) -> bool:
    v = os.environ.get(key, "").strip().lower()
    if not v:
        return default
    return v in ("1", "true", "yes", "on")


def load_config(env_path: str | os.PathLike | None = None) -> BridgeV2Config:
    """Carrega variaveis de ambiente e constroi BridgeV2Config tipado.

    Chame load_env_file() antes se quiser ler de um arquivo .env.
    Segredos (API keys) sao verificados apenas para presenca -- nunca incluidos.
    """
    load_env_file(env_path)

    try:
        llm_provider = LlmProvider(_env("NOISEBOT_LLM_PROVIDER", "openai"))
    except ValueError:
        log.warning("NOISEBOT_LLM_PROVIDER invalido, usando 'openai'")
        llm_provider = LlmProvider.OPENAI

    try:
        pipeline_mode = PipelineMode(_env("NOISEBOT_PIPELINE_MODE", "normal"))
    except ValueError:
        log.warning("NOISEBOT_PIPELINE_MODE invalido, usando 'normal'")
        pipeline_mode = PipelineMode.NORMAL

    try:
        log_level = LogLevel(_env("NOISEBOT_LOG_LEVEL", "INFO").upper())
    except ValueError:
        log_level = LogLevel.INFO

    return BridgeV2Config(
        transport=TransportConfig(
            host=_env("NOISEBOT_HOST") or None,
            port=_env_int("NOISEBOT_PORT", 9000),
            uart=_env("NOISEBOT_UART") or None,
            baudrate=_env_int("NOISEBOT_BAUDRATE", 1000000),
        ),
        llm=LlmConfig(
            provider=llm_provider,
            model=_env("NOISEBOT_LLM_MODEL", "gpt-4o-mini"),
            timeout_s=_env_float("NOISEBOT_LLM_TIMEOUT_S", 10.0),
            max_output_tokens=_env_int("NOISEBOT_LLM_MAX_OUTPUT_TOKENS", 140),
            max_reply_chars=_env_int("NOISEBOT_LLM_MAX_REPLY_CHARS", 180),
            openai_key_configured=bool(_env("OPENAI_API_KEY")),
            gemini_key_configured=bool(_env("GEMINI_API_KEY")),
        ),
        pipeline_mode=pipeline_mode,
        stt=SttConfig(
            model=_env("NOISEBOT_WHISPER_MODEL", "small"),
            backend=_env("NOISEBOT_WHISPER_BACKEND", "faster"),
            device=_env("NOISEBOT_WHISPER_DEVICE", "cpu"),
            compute_type=_env("NOISEBOT_WHISPER_COMPUTE_TYPE", "int8"),
        ),
        tts=TtsConfig(
            piper_executable=_env("NOISEBOT_PIPER_EXECUTABLE", "piper"),
            piper_model=_env("NOISEBOT_PIPER_MODEL", ""),
            cache_size=_env_int("NOISEBOT_TTS_CACHE_SIZE", 64),
            sample_rate=16000,
            target_peak=_env_int("NOISEBOT_TTS_TARGET_PEAK", 8000),
        ),
        audio=AudioConfig(
            chunk_samples=256,
            sample_rate=16000,
            min_transcribe_rms=_env_float("NOISEBOT_MIN_TRANSCRIBE_RMS", 140.0),
            min_transcribe_peak=_env_int("NOISEBOT_MIN_TRANSCRIBE_PEAK", 1600),
            min_utterance_samples=8000,
            max_no_speech_prob=0.75,
            min_avg_logprob=-1.10,
            max_compression_ratio=2.60,
        ),
        reconnect=ReconnectConfig(
            delay_s=_env_float("NOISEBOT_RECONNECT_DELAY_S", 1.0),
            max_delay_s=_env_float("NOISEBOT_RECONNECT_MAX_DELAY_S", 30.0),
            connect_timeout_s=_env_float("NOISEBOT_TCP_CONNECT_TIMEOUT_S", 5.0),
        ),
        ops=OpsConfig(
            port=_env_int("NOISEBOT_OPS_PORT", 8765),
            token_configured=bool(_env("NOISEBOT_OPS_TOKEN")),
        ),
        log_level=log_level,
        dry_run=_env_bool("NOISEBOT_DRY_RUN", False),
        replay_path=_env("NOISEBOT_REPLAY") or None,
    )
