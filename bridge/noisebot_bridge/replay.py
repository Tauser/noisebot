from __future__ import annotations

from dataclasses import asdict, dataclass
import logging
from pathlib import Path
import wave

import numpy as np

from .transport import NullTransport
from .voice_session import VoiceSessionResult, VoiceSessionRuntime, VoiceSnapshot

log = logging.getLogger("noisebot_bridge.replay")


@dataclass(frozen=True)
class ReplayResult:
    path: str
    samples: int
    duration_s: float
    chunks: int
    session: VoiceSessionResult

    def to_dict(self) -> dict:
        data = asdict(self)
        data["duration_s"] = round(self.duration_s, 3)
        return data


@dataclass(frozen=True)
class ReplayBatchResult:
    root: str
    total: int
    outcomes: dict[str, int]
    results: list[ReplayResult]

    def to_dict(self) -> dict:
        return {
            "root": self.root,
            "total": self.total,
            "outcomes": dict(sorted(self.outcomes.items())),
            "results": [r.to_dict() for r in self.results],
        }


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


def run_replay(path: str, stt, llm, tts, dry_run: bool, intent_router=None) -> ReplayResult:
    pcm = load_audio(path)
    chunks = [pcm[i : i + 256].copy() for i in range(0, len(pcm), 256)]
    if chunks and len(chunks[-1]) < 256:
        chunks[-1] = np.pad(chunks[-1], (0, 256 - len(chunks[-1]))).astype(np.int16)
    rms_values = [float(np.sqrt(np.mean(c.astype(np.float32) ** 2))) for c in chunks] if chunks else [0.0]
    transport = NullTransport()
    runtime = VoiceSessionRuntime(transport, stt, llm, tts, dry_run=dry_run, intent_router=intent_router)
    snapshot = VoiceSnapshot(
        session_id=1,
        audio_chunks=chunks,
        avg_rms=sum(rms_values) / max(1, len(rms_values)),
        duration_s=len(pcm) / 16000.0,
        end_reason="replay",
    )
    log.info("REPLAY arquivo=%s samples=%d dur=%.1fs chunks=%d", path, len(pcm), snapshot.duration_s, len(chunks))
    session = runtime.handle_voice_end(snapshot)
    result = ReplayResult(
        path=str(Path(path)),
        samples=len(pcm),
        duration_s=snapshot.duration_s,
        chunks=len(chunks),
        session=session,
    )
    log.info(
        "REPLAY_RESULT session_id=%d route=%s outcome=%s detail=%s end_reason=%s",
        session.session_id,
        session.route,
        session.outcome,
        session.outcome_detail,
        session.end_reason,
    )
    return result


def iter_replay_audio_files(root: str) -> list[Path]:
    p = Path(root)
    if p.is_file():
        return [p]
    files = [*p.glob("*.wav"), *p.glob("*.pcm")]
    return sorted(files)


def run_replay_batch(root: str, stt, llm, tts, dry_run: bool, intent_router=None) -> ReplayBatchResult:
    results = [
        run_replay(str(path), stt, llm, tts, dry_run=dry_run, intent_router=intent_router)
        for path in iter_replay_audio_files(root)
    ]
    outcomes: dict[str, int] = {}
    for result in results:
        outcome = result.session.outcome
        outcomes[outcome] = outcomes.get(outcome, 0) + 1
    return ReplayBatchResult(root=str(Path(root)), total=len(results), outcomes=outcomes, results=results)
