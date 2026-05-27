"""Typed NoiseBot server configuration loaded from environment variables."""

from __future__ import annotations

import logging
import os
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Any

log = logging.getLogger(__name__)

DEFAULT_LLM_PROVIDER = "ollama"
DEFAULT_LLM_MODELS = {
    "openai": "gpt-4o-mini",
    "gemini": "gemini-2.5-flash",
    "ollama": "qwen2.5:7b",
    "none": "none",
}


class LlmProvider(str, Enum):
    OPENAI = "openai"
    GEMINI = "gemini"
    OLLAMA = "ollama"
    NONE = "none"


class PipelineMode(str, Enum):
    NORMAL = "normal"
    LOCAL_ONLY = "local_only"
    DEGRADED = "degraded"
    REALTIME = "realtime"


class LogLevel(str, Enum):
    DEBUG = "DEBUG"
    INFO = "INFO"
    WARNING = "WARNING"
    ERROR = "ERROR"


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
    temperature: float
    max_output_tokens: int
    max_reply_chars: int
    ollama_base_url: str
    ollama_think: bool
    openai_key_configured: bool
    gemini_key_configured: bool


@dataclass(frozen=True)
class SttConfig:
    model: str
    backend: str
    device: str
    compute_type: str
    beam_size: int = 3


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
    max_utterance_samples: int
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
    token_configured: bool


@dataclass(frozen=True)
class NoiseBotServerConfig:
    """Complete server config without secret values."""

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

    def safe_dict(self) -> dict[str, Any]:
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
                "temperature": self.llm.temperature,
                "max_output_tokens": self.llm.max_output_tokens,
                "ollama_base_url": self.llm.ollama_base_url,
                "ollama_think": self.llm.ollama_think,
                "openai_key_configured": self.llm.openai_key_configured,
                "gemini_key_configured": self.llm.gemini_key_configured,
            },
            "pipeline_mode": self.pipeline_mode.value,
            "stt": {
                "model": self.stt.model,
                "backend": self.stt.backend,
                "device": self.stt.device,
                "compute_type": self.stt.compute_type,
                "beam_size": self.stt.beam_size,
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
                "min_utterance_samples": self.audio.min_utterance_samples,
                "max_utterance_samples": self.audio.max_utterance_samples,
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


def _default_env_candidates() -> list[Path]:
    package_env = Path(__file__).resolve().parents[1] / ".env"
    cwd = Path.cwd()
    candidates = [
        package_env,
        cwd / ".env",
    ]
    for parent in Path(__file__).resolve().parents:
        candidates.append(parent / ".env")
    return candidates


def find_env_file(path: str | os.PathLike[str] | None = None) -> Path | None:
    candidates = [Path(path)] if path is not None else _default_env_candidates()
    seen: set[Path] = set()
    for candidate in candidates:
        resolved = candidate.resolve()
        if resolved in seen:
            continue
        seen.add(resolved)
        if resolved.exists():
            return resolved
    return None


def load_env_file(path: str | os.PathLike[str] | None = None) -> Path | None:
    env_path = find_env_file(path)
    if env_path is None:
        return None

    log.info("Config: .env carregado de %s", env_path)

    for raw_line in env_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        if key and not os.environ.get(key):
            os.environ[key] = value
    return env_path


def _env(key: str, default: str = "") -> str:
    return os.environ.get(key, default)


def _env_int(key: str, default: int) -> int:
    try:
        return int(os.environ.get(key, default))
    except (TypeError, ValueError):
        return default


def _env_float(key: str, default: float) -> float:
    try:
        return float(os.environ.get(key, default))
    except (TypeError, ValueError):
        return default


def _env_bool(key: str, default: bool = False) -> bool:
    value = os.environ.get(key, "").strip().lower()
    if not value:
        return default
    return value in ("1", "true", "yes", "on")


def load_config(env_path: str | os.PathLike[str] | None = None) -> NoiseBotServerConfig:
    load_env_file(env_path)

    try:
        llm_provider = LlmProvider(_env("NOISEBOT_LLM_PROVIDER", DEFAULT_LLM_PROVIDER))
    except ValueError:
        log.warning("NOISEBOT_LLM_PROVIDER invalido, usando '%s'", DEFAULT_LLM_PROVIDER)
        llm_provider = LlmProvider(DEFAULT_LLM_PROVIDER)

    try:
        pipeline_mode = PipelineMode(_env("NOISEBOT_PIPELINE_MODE", "normal"))
    except ValueError:
        log.warning("NOISEBOT_PIPELINE_MODE invalido, usando 'normal'")
        pipeline_mode = PipelineMode.NORMAL

    try:
        log_level = LogLevel(_env("NOISEBOT_LOG_LEVEL", "INFO").upper())
    except ValueError:
        log_level = LogLevel.INFO

    return NoiseBotServerConfig(
        transport=TransportConfig(
            host=_env("NOISEBOT_HOST") or None,
            port=_env_int("NOISEBOT_PORT", 9000),
            uart=_env("NOISEBOT_UART") or None,
            baudrate=_env_int("NOISEBOT_BAUDRATE", 1000000),
        ),
        llm=LlmConfig(
            provider=llm_provider,
            model=_env("NOISEBOT_LLM_MODEL", DEFAULT_LLM_MODELS[llm_provider.value]),
            timeout_s=_env_float("NOISEBOT_LLM_TIMEOUT_S", 10.0),
            temperature=_env_float("NOISEBOT_LLM_TEMPERATURE", 0.85),
            max_output_tokens=_env_int("NOISEBOT_LLM_MAX_OUTPUT_TOKENS", 256),
            max_reply_chars=_env_int("NOISEBOT_LLM_MAX_REPLY_CHARS", 180),
            ollama_base_url=_env("NOISEBOT_OLLAMA_BASE_URL", "http://127.0.0.1:11434"),
            ollama_think=_env_bool("NOISEBOT_OLLAMA_THINK", False),
            openai_key_configured=bool(_env("OPENAI_API_KEY")),
            gemini_key_configured=bool(_env("GEMINI_API_KEY")),
        ),
        pipeline_mode=pipeline_mode,
        stt=SttConfig(
            model=_env("NOISEBOT_WHISPER_MODEL", "small"),
            backend=_env("NOISEBOT_WHISPER_BACKEND", "faster"),
            device=_env("NOISEBOT_WHISPER_DEVICE", "cpu"),
            compute_type=_env("NOISEBOT_WHISPER_COMPUTE_TYPE", "int8"),
            beam_size=_env_int("NOISEBOT_WHISPER_BEAM_SIZE", 5),
        ),
        tts=TtsConfig(
            piper_executable=_env("NOISEBOT_PIPER_EXECUTABLE", "piper"),
            piper_model=_env("NOISEBOT_PIPER_MODEL", ""),
            cache_size=_env_int("NOISEBOT_TTS_CACHE_SIZE", 64),
            sample_rate=16000,
            target_peak=_env_int("NOISEBOT_TTS_TARGET_PEAK", 12000),
        ),
        audio=AudioConfig(
            chunk_samples=256,
            sample_rate=16000,
            min_transcribe_rms=_env_float("NOISEBOT_MIN_TRANSCRIBE_RMS", 140.0),
            min_transcribe_peak=_env_int("NOISEBOT_MIN_TRANSCRIBE_PEAK", 1600),
            min_utterance_samples=8000,
            max_utterance_samples=_env_int("NOISEBOT_MAX_UTTERANCE_SAMPLES", 192000),
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


__all__ = [
    "AudioConfig",
    "DEFAULT_LLM_MODELS",
    "DEFAULT_LLM_PROVIDER",
    "LlmConfig",
    "LlmProvider",
    "LogLevel",
    "NoiseBotServerConfig",
    "OpsConfig",
    "PipelineMode",
    "ReconnectConfig",
    "SttConfig",
    "TransportConfig",
    "TtsConfig",
    "find_env_file",
    "load_config",
    "load_env_file",
]
