from __future__ import annotations

from dataclasses import dataclass
import os


@dataclass(frozen=True)
class BridgeConfig:
    host: str | None = None
    port: int = 9000
    uart: str | None = None
    dry_run: bool = False
    replay: str | None = None
    llm: str = "gemini"
    fallback_llm: str = "none"
    whisper_model: str = os.environ.get("NOISEBOT_WHISPER_MODEL", "small")
    whisper_backend: str = os.environ.get("NOISEBOT_WHISPER_BACKEND", "faster")
    whisper_device: str = os.environ.get("NOISEBOT_WHISPER_DEVICE", "cpu")
    whisper_compute_type: str = os.environ.get("NOISEBOT_WHISPER_COMPUTE_TYPE", "int8")
    piper_model: str = os.environ.get("PIPER_MODEL", "pt_BR-faber-medium.onnx")
    reconnect_delay_s: float = 5.0


WHISPER_TARGET_PEAK = 0.86
WHISPER_MIN_PEAK_FOR_GAIN = 0.04
MIN_TRANSCRIBE_RMS = float(os.environ.get("NOISEBOT_MIN_TRANSCRIBE_RMS", "140"))
MIN_TRANSCRIBE_PEAK = int(os.environ.get("NOISEBOT_MIN_TRANSCRIBE_PEAK", "1600"))
LOG_TEXT_MAX_CHARS = 160

CHUNK_SAMPLES = 256
VOICE_TIMEOUT_S = 70.0
MIN_UTTERANCE_SAMPLES = 8000
MIN_UTTERANCE_RMS = 80.0
MAX_NO_SPEECH_PROB = 0.75
MAX_NO_SPEECH_PROB_DRY_RUN = 0.90
MIN_AVG_LOGPROB = -1.10
MAX_COMPRESSION_RATIO = 2.60
