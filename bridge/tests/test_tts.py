import unittest

import numpy as np

from noisebot_bridge.tts import limit_peak, resample_linear
from noisebot_bridge.config import TTS_TARGET_PEAK


class TtsAudioTests(unittest.TestCase):
    def test_resample_piper_rate_to_firmware_rate(self):
        pcm = np.arange(22050, dtype=np.int16)

        out = resample_linear(pcm, 22050, 16000)

        self.assertEqual(len(out), 16000)
        self.assertEqual(out.dtype, np.int16)

    def test_limit_peak_does_not_exceed_target(self):
        pcm = np.array([-32768, -16000, 0, 16000, 32767], dtype=np.int16)

        out = limit_peak(pcm, 12000)

        self.assertLessEqual(int(np.max(np.abs(out.astype(np.int32)))), 12000)
        self.assertEqual(out.dtype, np.int16)

    def test_default_tts_peak_is_conservative(self):
        self.assertLessEqual(TTS_TARGET_PEAK, 8000)


if __name__ == "__main__":
    unittest.main()
