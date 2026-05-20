from __future__ import annotations

from collections import OrderedDict
import json
import logging
from pathlib import Path
import subprocess

import numpy as np

from .config import TTS_CACHE_SIZE, TTS_SAMPLE_RATE, TTS_TARGET_PEAK

log = logging.getLogger("noisebot_bridge.tts")


def read_piper_sample_rate(model_path: str) -> int:
    cfg_path = Path(f"{model_path}.json")
    if not cfg_path.exists():
        return TTS_SAMPLE_RATE
    try:
        data = json.loads(cfg_path.read_text(encoding="utf-8"))
        return int(data.get("audio", {}).get("sample_rate", TTS_SAMPLE_RATE))
    except Exception as e:
        log.warning("Nao foi possivel ler sample_rate do Piper %s: %s", cfg_path, e)
        return TTS_SAMPLE_RATE


def resample_linear(pcm: np.ndarray, src_rate: int, dst_rate: int = TTS_SAMPLE_RATE) -> np.ndarray:
    if src_rate == dst_rate or pcm.size == 0:
        return pcm.astype(np.int16, copy=False)
    dst_len = max(1, int(round(pcm.size * float(dst_rate) / float(src_rate))))
    src_x = np.linspace(0.0, float(pcm.size - 1), num=pcm.size, dtype=np.float32)
    dst_x = np.linspace(0.0, float(pcm.size - 1), num=dst_len, dtype=np.float32)
    resampled = np.interp(dst_x, src_x, pcm.astype(np.float32))
    return np.clip(resampled, -32768, 32767).astype(np.int16)


def limit_peak(pcm: np.ndarray, target_peak: int = TTS_TARGET_PEAK) -> np.ndarray:
    if pcm.size == 0:
        return pcm.astype(np.int16, copy=False)
    peak = int(np.max(np.abs(pcm.astype(np.int32))))
    if peak <= 0 or peak <= target_peak:
        return pcm.astype(np.int16, copy=False)
    scaled = pcm.astype(np.float32) * (float(target_peak) / float(peak))
    return np.clip(scaled, -32768, 32767).astype(np.int16)


class PiperTts:
    def __init__(self, model_path: str):
        self.model_path = model_path
        self.sample_rate = read_piper_sample_rate(model_path)
        self._cache: OrderedDict[str, np.ndarray] = OrderedDict()

    def synthesize(self, text: str) -> np.ndarray:
        if not text:
            return np.zeros(0, dtype=np.int16)
        cached = self._cache.get(text)
        if cached is not None:
            self._cache.move_to_end(text)
            return cached.copy()
        cmd = ["piper", "--model", self.model_path, "--output_raw"]
        try:
            proc = subprocess.run(cmd, input=text.encode(), capture_output=True, check=False)
        except FileNotFoundError as e:
            raise RuntimeError("piper_binario_ausente") from e
        if proc.returncode != 0:
            stderr = proc.stderr.decode(errors="ignore").strip()
            raise RuntimeError(f"piper_falhou:{stderr or proc.returncode}")
        pcm = np.frombuffer(proc.stdout, dtype=np.int16)
        pcm = resample_linear(pcm, self.sample_rate, TTS_SAMPLE_RATE)
        pcm = limit_peak(pcm, TTS_TARGET_PEAK)
        if TTS_CACHE_SIZE > 0:
            self._cache[text] = pcm.copy()
            self._cache.move_to_end(text)
            while len(self._cache) > TTS_CACHE_SIZE:
                self._cache.popitem(last=False)
        return pcm
