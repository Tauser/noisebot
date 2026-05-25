from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path


def load_bridge_env(path: str | os.PathLike[str] | None = None) -> None:
    env_path = Path(path) if path is not None else Path(__file__).resolve().parents[1] / ".env"
    if not env_path.exists():
        return

    for raw_line in env_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        if key and key not in os.environ:
            os.environ[key] = value


@dataclass(frozen=True)
class BridgeConfig:
    host: str | None = None
    port: int = 9000
    uart: str | None = None
    dry_run: bool = False
    replay: str | None = None
    replay_json: bool = False
    local_intents: bool = True
    llm: str = "openai"
    fallback_llm: str = "none"
    whisper_model: str = os.environ.get("NOISEBOT_WHISPER_MODEL", "small")
    whisper_backend: str = os.environ.get("NOISEBOT_WHISPER_BACKEND", "faster")
    whisper_device: str = os.environ.get("NOISEBOT_WHISPER_DEVICE", "cpu")
    whisper_compute_type: str = os.environ.get("NOISEBOT_WHISPER_COMPUTE_TYPE", "int8")
    piper_model: str = os.environ.get("PIPER_MODEL", "pt_BR-faber-medium.onnx")
    reconnect_delay_s: float = 1.0


OPENAI_TIMEOUT_S = float(os.environ.get("NOISEBOT_OPENAI_TIMEOUT_S", "10"))
OPENAI_MAX_OUTPUT_TOKENS = int(os.environ.get("NOISEBOT_OPENAI_MAX_OUTPUT_TOKENS", "140"))
OPENAI_MAX_REPLY_CHARS = int(os.environ.get("NOISEBOT_OPENAI_MAX_REPLY_CHARS", "180"))

WHISPER_TARGET_PEAK = 0.86
WHISPER_MIN_PEAK_FOR_GAIN = 0.04
WHISPER_BEAM_SIZE = int(os.environ.get("NOISEBOT_WHISPER_BEAM_SIZE", "3"))
MIN_TRANSCRIBE_RMS = float(os.environ.get("NOISEBOT_MIN_TRANSCRIBE_RMS", "140"))
MIN_TRANSCRIBE_PEAK = int(os.environ.get("NOISEBOT_MIN_TRANSCRIBE_PEAK", "1600"))
LOG_TEXT_MAX_CHARS = 160

CHUNK_SAMPLES = 256
TTS_SAMPLE_RATE = 16000
TTS_TARGET_PEAK = int(os.environ.get("NOISEBOT_TTS_TARGET_PEAK", "8000"))
TTS_CACHE_SIZE = int(os.environ.get("NOISEBOT_TTS_CACHE_SIZE", "32"))
VOICE_TIMEOUT_S = 70.0
MIN_UTTERANCE_SAMPLES = 8000
MIN_UTTERANCE_RMS = 80.0
MAX_NO_SPEECH_PROB = 0.75
MAX_NO_SPEECH_PROB_DRY_RUN = 0.90
MIN_AVG_LOGPROB = -1.10
MAX_COMPRESSION_RATIO = 2.60
