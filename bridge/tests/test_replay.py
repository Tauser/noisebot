import json
import tempfile
import unittest
import wave
from pathlib import Path

import numpy as np

from noisebot_bridge.replay import iter_replay_audio_files, load_audio, run_replay, run_replay_batch
from noisebot_bridge.stt import SttResult


ROOT = Path(__file__).resolve().parents[2]
VOICE_SAMPLES = ROOT / "voice_samples"
VOICE_REPLAY_BASELINE = ROOT / "docs" / "VOICE_REPLAY_BASELINE.json"
REAL_GOOD_WAVS = (
    VOICE_SAMPLES / "bridge_tx_comando_curto_468s.wav",
    VOICE_SAMPLES / "raw_comando_curto_402s.wav",
)
REAL_BAD_WAVS = (
    VOICE_SAMPLES / "bridge_tx_ruido_ambiente_548s.wav",
    VOICE_SAMPLES / "bridge_tx_mesa_vibrando_616s.wav",
)


class FakeStt:
    ready = True
    calls = 0

    def empty_result(self):
        return SttResult(backend="fake")

    def transcribe(self, pcm):
        self.calls += 1
        return SttResult(
            text="que horas sao",
            no_speech_prob=0.01,
            avg_logprob=-0.2,
            compression_ratio=1.0,
            backend="fake",
        )


class NoSpeechStt(FakeStt):
    def transcribe(self, pcm):
        self.calls += 1
        return SttResult(
            text="",
            no_speech_prob=0.99,
            avg_logprob=-2.0,
            compression_ratio=0.0,
            backend="fake",
        )


class NoneLlm:
    ready = False


class FakeTts:
    def synthesize(self, text):
        return np.zeros(256, dtype=np.int16)


class ReplayTests(unittest.TestCase):
    def write_wav(self, path, pcm):
        with wave.open(str(path), "wb") as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(16000)
            wf.writeframes(np.asarray(pcm, dtype=np.int16).tobytes())

    def test_load_audio_accepts_mono_wav(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "sample.wav"
            pcm = np.full(9000, 2000, dtype=np.int16)
            self.write_wav(path, pcm)

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

    def test_replay_good_wav_reaches_stt(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "good.wav"
            pcm = np.full(9000, 2000, dtype=np.int16)
            self.write_wav(path, pcm)
            stt = FakeStt()

            result = run_replay(str(path), stt, NoneLlm(), FakeTts(), dry_run=True)

        data = result.to_dict()
        self.assertEqual(stt.calls, 1)
        self.assertEqual(data["samples"], 9000)
        self.assertEqual(data["session"]["outcome"], "dry_run_ok")

    def test_replay_silence_is_rejected_before_stt(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "silence.wav"
            pcm = np.zeros(9000, dtype=np.int16)
            self.write_wav(path, pcm)
            stt = FakeStt()

            result = run_replay(str(path), stt, NoneLlm(), FakeTts(), dry_run=False)

        data = result.to_dict()
        self.assertEqual(stt.calls, 0)
        self.assertEqual(data["session"]["outcome"], "audio_rejected")
        self.assertTrue(data["session"]["outcome_detail"].startswith("audio_baixo_"))

    def test_replay_no_speech_stt_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "noise.wav"
            pcm = np.full(9000, 2000, dtype=np.int16)
            self.write_wav(path, pcm)
            stt = NoSpeechStt()

            result = run_replay(str(path), stt, NoneLlm(), FakeTts(), dry_run=False)

        data = result.to_dict()
        self.assertEqual(stt.calls, 1)
        self.assertEqual(data["session"]["outcome"], "stt_rejected")

    def test_replay_real_good_voice_samples_reach_stt(self):
        for path in REAL_GOOD_WAVS:
            with self.subTest(path=path.name):
                self.assertTrue(path.exists(), f"fixture ausente: {path}")
                stt = FakeStt()

                result = run_replay(str(path), stt, NoneLlm(), FakeTts(), dry_run=True)

                data = result.to_dict()
                self.assertEqual(stt.calls, 1)
                self.assertGreater(data["samples"], 16000)
                self.assertEqual(data["session"]["outcome"], "dry_run_ok")

    def test_replay_real_bad_voice_samples_reject_no_speech(self):
        for path in REAL_BAD_WAVS:
            with self.subTest(path=path.name):
                self.assertTrue(path.exists(), f"fixture ausente: {path}")
                stt = NoSpeechStt()

                result = run_replay(str(path), stt, NoneLlm(), FakeTts(), dry_run=False)

                data = result.to_dict()
                self.assertEqual(stt.calls, 1)
                self.assertGreater(data["samples"], 16000)
                self.assertEqual(data["session"]["outcome"], "stt_rejected")

    def test_iter_replay_audio_files_returns_supported_files_sorted(self):
        files = iter_replay_audio_files(str(VOICE_SAMPLES))

        names = [p.name for p in files]
        self.assertIn("bridge_tx_comando_curto_468s.wav", names)
        self.assertIn("raw_comando_curto_402s.wav", names)
        self.assertEqual(names, sorted(names))

    def test_run_replay_batch_summarizes_outcomes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.write_wav(root / "a_good.wav", np.full(9000, 2000, dtype=np.int16))
            self.write_wav(root / "b_good.wav", np.full(9000, 2000, dtype=np.int16))
            stt = FakeStt()

            result = run_replay_batch(str(root), stt, NoneLlm(), FakeTts(), dry_run=True)

        data = result.to_dict()
        self.assertEqual(stt.calls, 2)
        self.assertEqual(data["total"], 2)
        self.assertEqual(data["outcomes"], {"dry_run_ok": 2})
        self.assertEqual([Path(r["path"]).name for r in data["results"]], ["a_good.wav", "b_good.wav"])

    def test_voice_replay_baseline_matches_real_fixtures(self):
        baseline = json.loads(VOICE_REPLAY_BASELINE.read_text(encoding="utf-8"))

        self.assertEqual(baseline["sample_rate_hz"], 16000)
        self.assertEqual(baseline["source_dir"], "voice_samples")

        for sample in baseline["good_samples"]:
            with self.subTest(file=sample["file"]):
                path = VOICE_SAMPLES / sample["file"]
                self.assertTrue(path.exists(), f"fixture ausente: {path}")
                result = run_replay(str(path), FakeStt(), NoneLlm(), FakeTts(), dry_run=True)

                self.assertEqual(result.session.outcome, sample["expected_outcome"])

        for sample in baseline["rejected_samples"]:
            with self.subTest(file=sample["file"]):
                path = VOICE_SAMPLES / sample["file"]
                self.assertTrue(path.exists(), f"fixture ausente: {path}")
                result = run_replay(str(path), NoSpeechStt(), NoneLlm(), FakeTts(), dry_run=False)

                self.assertEqual(result.session.outcome, sample["expected_outcome"])


if __name__ == "__main__":
    unittest.main()
