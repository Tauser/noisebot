import unittest

import numpy as np

from noisebot_bridge.protocol import (
    MSG_GAZE,
    MSG_SAY,
    MSG_SESSION,
    SESSION_THINKING_START,
    SESSION_TRANSCRIBE_START,
    SESSION_TTS_START,
    SESSION_TTS_STOP,
    decode_frames,
)
from noisebot_bridge.stt import SttResult
from noisebot_bridge.transport import NullTransport
from noisebot_bridge.voice_session import VoiceSessionRuntime, VoiceSnapshot, classify_session_outcome
from noisebot_bridge.intent_router import DeviceCommand, LocalIntentResult


class FakeStt:
    ready = True

    def empty_result(self):
        return SttResult(backend="fake")

    def transcribe(self, pcm):
        return SttResult(
            text="que horas sao agora",
            no_speech_prob=0.01,
            avg_logprob=-0.2,
            compression_ratio=1.0,
            elapsed_ms=1.0,
            backend="fake",
        )


class LowConfidenceStt(FakeStt):
    def transcribe(self, pcm):
        return SttResult(
            text="O que?",
            no_speech_prob=0.01,
            avg_logprob=-1.5,
            compression_ratio=1.0,
            elapsed_ms=1.0,
            backend="fake",
        )


class NoneLlm:
    ready = False

    def __init__(self):
        self.calls = 0

    def generate(self, text, status):
        self.calls += 1
        raise AssertionError("LLM nao deveria ser chamada")


class ReadyLlm:
    ready = True

    def __init__(self):
        self.calls = 0

    def generate(self, text, status):
        self.calls += 1
        from noisebot_bridge.llm import LlmResult

        return LlmResult(
            reply="Resposta pela LLM.",
            expression_id=2,
            action=0,
            emot_event=2,
            provider="fake",
            model="fake",
        )


class EmptyIntentRouter:
    def route(self, text, status=None):
        return None


class FakeIntentRouter:
    def route(self, text, status=None):
        return LocalIntentResult(
            intent="local_time",
            confidence=0.9,
            reply="Agora são 8 horas.",
            expression_id=2,
            action=0,
            emot_event=2,
        )


class FakeDeviceIntentRouter:
    def route(self, text, status=None):
        return LocalIntentResult(
            intent="local_device_move",
            confidence=0.9,
            reply="Olhando para esquerda.",
            expression_id=2,
            action=0,
            emot_event=2,
            device_commands=(DeviceCommand("look", {"direction": "esquerda"}, supported=True),),
        )


class FakeTts:
    def synthesize(self, text):
        return np.zeros(256, dtype=np.int16)


class FailingTts:
    def synthesize(self, text):
        raise RuntimeError("piper_falhou:voz_ausente")


class VoiceSessionTests(unittest.TestCase):
    def test_classifies_stt_rejection_without_session_error(self):
        outcome, detail, error = classify_session_outcome("logprob_-1.43", "discard", "silence")

        self.assertEqual(outcome, "stt_rejected")
        self.assertEqual(detail, "logprob_-1.43")
        self.assertIsNone(error)

    def test_dry_run_transcribes_and_sends_single_ack(self):
        transport = NullTransport()
        events = []
        runtime = VoiceSessionRuntime(
            transport,
            FakeStt(),
            NoneLlm(),
            FakeTts(),
            dry_run=True,
            session_event_cb=lambda event, session_id, **fields: events.append(event),
        )
        audio = np.full(9000, 2000, dtype=np.int16)
        snapshot = VoiceSnapshot(
            session_id=1,
            audio_chunks=[audio],
            avg_rms=1200.0,
            duration_s=0.6,
            end_reason="replay",
        )

        runtime.handle_voice_end(snapshot)

        self.assertEqual(len(transport.sent), 1)
        self.assertIn(MSG_SAY.to_bytes(1, "little"), transport.sent[0][1])
        self.assertIn(SESSION_TRANSCRIBE_START, events)
        self.assertNotIn(SESSION_TTS_START, events)

    def test_real_mode_local_intent_skips_llm_and_speaks(self):
        transport = NullTransport()
        llm = NoneLlm()
        runtime = VoiceSessionRuntime(
            transport,
            FakeStt(),
            llm,
            FakeTts(),
            dry_run=False,
            intent_router=FakeIntentRouter(),
        )
        audio = np.full(9000, 2000, dtype=np.int16)
        snapshot = VoiceSnapshot(
            session_id=1,
            audio_chunks=[audio],
            avg_rms=1200.0,
            duration_s=0.6,
            end_reason="replay",
        )

        runtime.handle_voice_end(snapshot)

        self.assertEqual(llm.calls, 0)
        self.assertGreaterEqual(len(transport.sent), 4)

    def test_local_intent_emits_tts_events(self):
        transport = NullTransport()
        events = []
        runtime = VoiceSessionRuntime(
            transport,
            FakeStt(),
            NoneLlm(),
            FakeTts(),
            dry_run=False,
            intent_router=FakeIntentRouter(),
            session_event_cb=lambda event, session_id, **fields: events.append((event, fields.get("reason"))),
        )
        audio = np.full(9000, 2000, dtype=np.int16)
        snapshot = VoiceSnapshot(
            session_id=1,
            audio_chunks=[audio],
            avg_rms=1200.0,
            duration_s=0.6,
            end_reason="replay",
        )

        runtime.handle_voice_end(snapshot)

        self.assertIn((SESSION_TTS_START, None), events)
        self.assertIn((SESSION_TTS_STOP, "ok"), events)

    def test_llm_unavailable_returns_session_error_reason(self):
        transport = NullTransport()
        runtime = VoiceSessionRuntime(
            transport,
            FakeStt(),
            NoneLlm(),
            FakeTts(),
            dry_run=False,
            intent_router=EmptyIntentRouter(),
        )
        audio = np.full(9000, 2000, dtype=np.int16)
        snapshot = VoiceSnapshot(
            session_id=1,
            audio_chunks=[audio],
            avg_rms=2000.0,
            duration_s=0.6,
            end_reason="replay",
        )

        result = runtime.handle_voice_end(snapshot)

        self.assertEqual(result.outcome, "llm_unavailable")
        self.assertEqual(result.outcome_detail, "llm_indisponivel")
        self.assertEqual(result.error_reason, "llm_unavailable")
        self.assertEqual(result.route, "error")

    def test_low_confidence_stt_returns_standard_rejection(self):
        transport = NullTransport()
        runtime = VoiceSessionRuntime(transport, LowConfidenceStt(), NoneLlm(), FakeTts(), dry_run=True)
        audio = np.full(9000, 2000, dtype=np.int16)
        snapshot = VoiceSnapshot(
            session_id=1,
            audio_chunks=[audio],
            avg_rms=1200.0,
            duration_s=0.6,
            end_reason="silence",
        )

        result = runtime.handle_voice_end(snapshot)

        self.assertEqual(result.outcome, "stt_rejected")
        self.assertEqual(result.outcome_detail, "logprob_-1.50")
        self.assertIsNone(result.error_reason)

    def test_session_events_can_be_sent_as_protocol_frames(self):
        from noisebot_bridge.runtime import BridgeRuntime

        transport = NullTransport()
        runtime = BridgeRuntime(transport, FakeStt(), NoneLlm(), FakeTts(), dry_run=True)

        runtime.log_session_event(SESSION_TRANSCRIBE_START, 3)

        frames = decode_frames(bytearray(transport.sent[-1][1]))
        self.assertEqual(frames[0][0], MSG_SESSION)

    def test_real_mode_device_intent_dispatches_command(self):
        transport = NullTransport()
        runtime = VoiceSessionRuntime(
            transport,
            FakeStt(),
            NoneLlm(),
            FakeTts(),
            dry_run=False,
            intent_router=FakeDeviceIntentRouter(),
        )
        audio = np.full(9000, 2000, dtype=np.int16)
        snapshot = VoiceSnapshot(
            session_id=1,
            audio_chunks=[audio],
            avg_rms=1200.0,
            duration_s=0.6,
            end_reason="replay",
        )

        runtime.handle_voice_end(snapshot)

        decoded = []
        for _, frame in transport.sent:
            decoded.extend(decode_frames(bytearray(frame)))
        self.assertIn(MSG_GAZE, [msg_type for msg_type, _ in decoded])

    def test_local_intent_tts_failure_sends_ack(self):
        transport = NullTransport()
        runtime = VoiceSessionRuntime(
            transport,
            FakeStt(),
            NoneLlm(),
            FailingTts(),
            dry_run=False,
            intent_router=FakeIntentRouter(),
        )
        audio = np.full(9000, 2000, dtype=np.int16)
        snapshot = VoiceSnapshot(
            session_id=1,
            audio_chunks=[audio],
            avg_rms=1200.0,
            duration_s=0.6,
            end_reason="replay",
        )

        runtime.handle_voice_end(snapshot)

        frames = decode_frames(bytearray(transport.sent[-1][1]))
        self.assertEqual(frames[-1], (MSG_SAY, b""))


    def test_unknown_text_uses_llm_when_available(self):
        transport = NullTransport()
        llm = ReadyLlm()
        runtime = VoiceSessionRuntime(
            transport,
            FakeStt(),
            llm,
            FakeTts(),
            dry_run=False,
            intent_router=EmptyIntentRouter(),
        )
        audio = np.full(9000, 2000, dtype=np.int16)
        snapshot = VoiceSnapshot(
            session_id=1,
            audio_chunks=[audio],
            avg_rms=2000.0,
            duration_s=0.6,
            end_reason="replay",
        )

        runtime.handle_voice_end(snapshot)

        self.assertEqual(llm.calls, 1)
        self.assertGreaterEqual(len(transport.sent), 4)

    def test_unknown_text_emits_thinking_start(self):
        transport = NullTransport()
        llm = ReadyLlm()
        events = []
        runtime = VoiceSessionRuntime(
            transport,
            FakeStt(),
            llm,
            FakeTts(),
            dry_run=False,
            intent_router=EmptyIntentRouter(),
            session_event_cb=lambda event, session_id, **fields: events.append(event),
        )
        audio = np.full(9000, 2000, dtype=np.int16)
        snapshot = VoiceSnapshot(
            session_id=1,
            audio_chunks=[audio],
            avg_rms=2000.0,
            duration_s=0.6,
            end_reason="replay",
        )

        runtime.handle_voice_end(snapshot)

        self.assertIn(SESSION_THINKING_START, events)

    def test_unknown_text_without_llm_speaks_degraded_error(self):
        transport = NullTransport()
        runtime = VoiceSessionRuntime(
            transport,
            FakeStt(),
            NoneLlm(),
            FakeTts(),
            dry_run=False,
            intent_router=EmptyIntentRouter(),
        )
        audio = np.full(9000, 2000, dtype=np.int16)
        snapshot = VoiceSnapshot(
            session_id=1,
            audio_chunks=[audio],
            avg_rms=2000.0,
            duration_s=0.6,
            end_reason="replay",
        )

        runtime.handle_voice_end(snapshot)

        self.assertGreaterEqual(len(transport.sent), 4)


if __name__ == "__main__":
    unittest.main()
