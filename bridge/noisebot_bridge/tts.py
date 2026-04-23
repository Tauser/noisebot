from __future__ import annotations

import logging
import subprocess

import numpy as np

log = logging.getLogger("noisebot_bridge.tts")


class PiperTts:
    def __init__(self, model_path: str):
        self.model_path = model_path

    def synthesize(self, text: str) -> np.ndarray:
        if not text:
            return np.zeros(0, dtype=np.int16)
        cmd = ["piper", "--model", self.model_path, "--output_raw"]
        try:
            proc = subprocess.run(cmd, input=text.encode(), capture_output=True, check=False)
        except FileNotFoundError as e:
            raise RuntimeError("piper_binario_ausente") from e
        if proc.returncode != 0:
            stderr = proc.stderr.decode(errors="ignore").strip()
            raise RuntimeError(f"piper_falhou:{stderr or proc.returncode}")
        return np.frombuffer(proc.stdout, dtype=np.int16)
