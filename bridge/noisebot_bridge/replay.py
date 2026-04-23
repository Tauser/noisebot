from __future__ import annotations

import logging
from pathlib import Path
import time
import wave

import numpy as np

from .transport import NullTransport
from .voice_session import VoiceSessionRuntime, VoiceSnapshot

log = logging.getLogger("noisebot_bridge.replay")


def load_audio(path: str) -> np.ndarray:
    p = Path(path)
    if p.suffix.lower() == ".wav":
        with wave.open(str(p), "rb") as wf:
            if wf.getsampwidth() != 2:
                raise ValueError("Replay WAV precisa ser PCM 16-bit")
            channels = wf.getnchannels()
            raw = wf.readframes(wf.getnframes())
            pcm = np.frombuffer(raw, dtype=np.int16)
            if channels > 1:
                pcm = pcm.reshape(-1, channels)[:, 0]
            return pcm.astype(np.int16).copy()
    return np.fromfile(str(p), dtype=np.int16)


def run_replay(path: str, stt, llm, tts, dry_run: bool):
    pcm = load_audio(path)
    chunks = [pcm[i : i + 256].copy() for i in range(0, len(pcm), 256)]
    if chunks and len(chunks[-1]) < 256:
        chunks[-1] = np.pad(chunks[-1], (0, 256 - len(chunks[-1]))).astype(np.int16)
    rms_values = [float(np.sqrt(np.mean(c.astype(np.float32) ** 2))) for c in chunks] if chunks else [0.0]
    transport = NullTransport()
    runtime = VoiceSessionRuntime(transport, stt, llm, tts, dry_run=dry_run)
    snapshot = VoiceSnapshot(
        session_id=1,
        audio_chunks=chunks,
        avg_rms=sum(rms_values) / max(1, len(rms_values)),
        duration_s=len(pcm) / 16000.0,
        end_reason="replay",
    )
    log.info("REPLAY arquivo=%s samples=%d dur=%.1fs chunks=%d", path, len(pcm), snapshot.duration_s, len(chunks))
    runtime.handle_voice_end(snapshot)
