import tempfile
import unittest
import wave
from pathlib import Path

import numpy as np

from noisebot_bridge.replay import load_audio, run_replay
from noisebot_bridge.stt import SttResult


class FakeStt:
    ready = True

    def empty_result(self):
        return SttResult(backend="fake")

    def transcribe(self, pcm):
        return SttResult(
            text="que horas sao",
            no_speech_prob=0.01,
            avg_logprob=-0.2,
            compression_ratio=1.0,
            backend="fake",
        )


class NoneLlm:
    ready = False


class FakeTts:
    def synthesize(self, text):
        return np.zeros(256, dtype=np.int16)


class ReplayTests(unittest.TestCase):
    def test_load_audio_accepts_mono_wav(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "sample.wav"
            pcm = np.full(9000, 2000, dtype=np.int16)
            with wave.open(str(path), "wb") as wf:
                wf.setnchannels(1)
                wf.setsampwidth(2)
                wf.setframerate(16000)
                wf.writeframes(pcm.tobytes())

            loaded = load_audio(str(path))

        self.assertEqual(loaded.dtype, np.int16)
        self.assertEqual(len(loaded), len(pcm))
        self.assertEqual(int(loaded[0]), 2000)

    def test_run_replay_returns_structured_result(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "sample.pcm"
            pcm = np.full(9000, 2000, dtype=np.int16)
            pcm.tofile(path)

            result = run_replay(str(path), FakeStt(), NoneLlm(), FakeTts(), dry_run=True)

        data = result.to_dict()
        self.assertEqual(data["samples"], 9000)
        self.assertEqual(data["chunks"], 36)
        self.assertEqual(data["session"]["session_id"], 1)
        self.assertEqual(data["session"]["route"], "discard")
        self.assertEqual(data["session"]["outcome"], "dry_run_ok")


if __name__ == "__main__":
    unittest.main()
