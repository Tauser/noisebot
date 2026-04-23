from __future__ import annotations

from dataclasses import dataclass
import logging
import time

import numpy as np

from .config import WHISPER_MIN_PEAK_FOR_GAIN, WHISPER_TARGET_PEAK

log = logging.getLogger("noisebot_bridge.stt")


@dataclass
class SttResult:
    text: str = ""
    no_speech_prob: float = 0.0
    avg_logprob: float = 0.0
    compression_ratio: float = 0.0
    elapsed_ms: float = 0.0
    gain: float = 1.0
    peak_in: float = 0.0
    backend: str = "openai"


class WhisperStt:
    def __init__(self, model_name: str, backend: str, device: str, compute_type: str):
        self.model_name = model_name
        self.backend = backend.lower()
        self.device = device
        self.compute_type = compute_type
        self.model = None

    def init(self):
        if self.backend == "faster":
            from faster_whisper import WhisperModel

            log.info("Carregando faster-whisper %s (%s/%s)...", self.model_name, self.device, self.compute_type)
            self.model = WhisperModel(self.model_name, device=self.device, compute_type=self.compute_type)
        else:
            import whisper

            log.info("Carregando Whisper %s...", self.model_name)
            self.model = whisper.load_model(self.model_name)
            self.backend = "openai"
        log.info("Whisper pronto")

    @property
    def ready(self) -> bool:
        return self.model is not None

    def empty_result(self) -> SttResult:
        return SttResult(backend=self.backend)

    def transcribe(self, pcm_int16: np.ndarray) -> SttResult:
        if self.model is None:
            raise RuntimeError("Whisper não inicializado")

        audio = pcm_int16.astype(np.float32) / 32768.0
        peak_in = float(np.max(np.abs(audio))) if audio.size else 0.0
        gain = 1.0
        if peak_in >= WHISPER_MIN_PEAK_FOR_GAIN and peak_in < WHISPER_TARGET_PEAK:
            gain = min(WHISPER_TARGET_PEAK / peak_in, 12.0)
            audio = np.clip(audio * gain, -0.98, 0.98)

        t0 = time.perf_counter()
        if self.backend == "faster":
            segments, _info = self.model.transcribe(
                audio,
                language="pt",
                task="transcribe",
                beam_size=1,
                best_of=1,
                temperature=0.0,
                condition_on_previous_text=False,
                initial_prompt="Comandos em português brasileiro para um robô chamado NoiseBot.",
                vad_filter=False,
            )
            segs = list(segments)
            text = " ".join(s.text.strip() for s in segs).strip()
            if segs:
                no_speech = max(float(getattr(s, "no_speech_prob", 0.0) or 0.0) for s in segs)
                avg_logprob = float(np.mean([float(getattr(s, "avg_logprob", -10.0) or -10.0) for s in segs]))
                compression = max(float(getattr(s, "compression_ratio", 0.0) or 0.0) for s in segs)
            else:
                no_speech = 1.0
                avg_logprob = -10.0
                compression = 999.0
        else:
            result = self.model.transcribe(
                audio,
                language="pt",
                task="transcribe",
                fp16=False,
                temperature=0.0,
                beam_size=1,
                best_of=1,
                condition_on_previous_text=False,
                initial_prompt="Comandos em português brasileiro para um robô chamado NoiseBot.",
            )
            segs = result.get("segments", [])
            text = result.get("text", "").strip()
            if segs:
                no_speech = max(float(s.get("no_speech_prob", 0.0)) for s in segs)
                avg_logprob = float(np.mean([float(s.get("avg_logprob", -10.0)) for s in segs]))
                compression = max(float(s.get("compression_ratio", 0.0)) for s in segs)
            else:
                no_speech = 1.0
                avg_logprob = -10.0
                compression = 999.0

        elapsed_ms = (time.perf_counter() - t0) * 1000.0
        return SttResult(
            text=text,
            no_speech_prob=no_speech,
            avg_logprob=avg_logprob,
            compression_ratio=compression,
            elapsed_ms=elapsed_ms,
            gain=gain,
            peak_in=peak_in,
            backend=self.backend,
        )
