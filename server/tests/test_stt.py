from __future__ import annotations

import importlib


class _Segment:
    text = "Comandos em portugues em portugues em portugues"
    no_speech_prob = 0.91
    avg_logprob = -0.49
    compression_ratio = 2.97


class _PromptLeakModel:
    def transcribe(self, *_args, **_kwargs):
        return [_Segment()], None


def test_whisper_no_speech_clears_prompt_leak_text() -> None:
    stt_module = importlib.import_module("noisebot_server.internal.agent.stt")
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")

    stt = stt_module.WhisperLocalSTT(initial_prompt=stt_module.DEFAULT_INITIAL_PROMPT)
    stt._model = _PromptLeakModel()

    text, quality, no_speech_prob, _avg_logprob, _compression_ratio = stt._transcribe_sync(
        (b"\x00\x10" * 3200)
    )

    assert text == ""
    assert quality is runtime.TranscriptQuality.NO_SPEECH
    assert no_speech_prob == 0.91
