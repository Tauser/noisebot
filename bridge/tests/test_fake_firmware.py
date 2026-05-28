import struct
import time
import unittest

import numpy as np

from noisebot_bridge.protocol import (
    MSG_AUDIO_CHUNK,
    MSG_EVENT,
    MSG_HELLO,
    MSG_SAY,
    MSG_SESSION,
    NB_EVT_VOICE_ACTIVITY_END,
    NB_EVT_VOICE_ACTIVITY_START,
    SESSION_FOLLOWUP_ARM,
    SESSION_LISTEN_START,
    SESSION_LISTEN_STOP,
    SESSION_SESSION_DONE,
    SESSION_SESSION_ERROR,
    SESSION_TTS_START,
    SESSION_TTS_STOP,
    SESSION_WAKE_DETECTED,
    decode_frames,
    decode_session_payload,
    encode_frame,
    encode_hello_payload,
)
from noisebot_bridge.runtime import BridgeRuntime
from noisebot_bridge.stt import SttResult


class ScriptedFirmwareTransport:
    def __init__(self):
        self.sent = []
        self.rx_buf = bytearray()

    def send(self, data: bytes):
        self.sent.append(data)

    def recv(self, max_bytes: int = 4096) -> bytes:
        return b""

    def close(self):
        return None

    def firmware_hello(self):
        payload = encode_hello_payload(
            {
                "protocol": "noisebot-bridge",
                "version": 2,
                "role": "firmware",
                "features": ["voice_events", "status", "session_events_v2"],
                "audio": {
                    "format": "pcm16",
                    "sample_rate": 16000,
                    "channels": 1,
                    "chunk_samples": 256,
                },
            }
        )
        return MSG_HELLO, payload

    def voice_start(self):
        return MSG_EVENT, struct.pack("<I", NB_EVT_VOICE_ACTIVITY_START)

    def voice_end(self, reason_code: int = 0):
        return MSG_EVENT, struct.pack("<II", NB_EVT_VOICE_ACTIVITY_END, reason_code)

    def audio_chunk(self, pcm):
        return MSG_AUDIO_CHUNK, np.asarray(pcm, dtype=np.int16).tobytes()

    def corrupt_frame(self):
        frame = bytearray(encode_frame(MSG_SESSION, b'{"bad":true}'))
        frame[-1] ^= 0xFF
        return bytes(frame)

    def deliver(self, runtime: BridgeRuntime, *messages):
        for item in messages:
            if isinstance(item, bytes):
                self.rx_buf.extend(item)
            else:
                msg_type, payload = item
                self.rx_buf.extend(encode_frame(msg_type, payload))
            for msg_type, payload in decode_frames(self.rx_buf):
                runtime.process_msg(msg_type, payload)

    def decoded_sent(self):
        frames = []
        for frame in self.sent:
            frames.extend(decode_frames(bytearray(frame)))
        return frames

    def sent_sessions(self):
        return [
            decode_session_payload(payload)
            for msg_type, payload in self.decoded_sent()
            if msg_type == MSG_SESSION
        ]

    def sent_session_events(self):
        return [event["event"] for event in self.sent_sessions()]


class FakeStt:
    ready = True

    def empty_result(self):
        return SttResult(backend="fake")

    def transcribe(self, pcm):
        return SttResult(
            text="me diga uma curiosidade",
            no_speech_prob=0.01,
            avg_logprob=-0.2,
            compression_ratio=1.0,
            elapsed_ms=1.0,
            backend="fake",
        )


class NoSpeechStt(FakeStt):
    def transcribe(self, pcm):
        return SttResult(
            text="",
            no_speech_prob=0.99,
            avg_logprob=-2.0,
            compression_ratio=0.0,
            elapsed_ms=1.0,
            backend="fake",
        )


class NoneLlm:
    ready = False

    def generate(self, text, status):
        raise AssertionError("LLM nao deveria ser chamada")


class FakeTts:
    def synthesize(self, text):
        return np.full(256, 1000, dtype=np.int16)


class LongFakeTts:
    def synthesize(self, text):
        return np.full(256 * 6, 1000, dtype=np.int16)


class FailingTts:
    def synthesize(self, text):
        raise RuntimeError("tts_fake_failure")


class FakeIntentRouter:
    def route(self, text, status=None):
        from noisebot_bridge.intent_router import LocalIntentResult

        return LocalIntentResult(
            intent="local_test",
            confidence=0.9,
            reply="Curiosidade local.",
            expression_id=2,
            action=0,
            emot_event=2,
        )


def wait_for(predicate, timeout_s=1.0):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(0.01)
    return predicate()


class FakeFirmwareProtocolTests(unittest.TestCase):
    def make_runtime(self, stt=None, tts=None, intent_router=None):
        transport = ScriptedFirmwareTransport()
        runtime = BridgeRuntime(
            transport,
            stt or FakeStt(),
            NoneLlm(),
            tts or FakeTts(),
            dry_run=False,
            intent_router=intent_router or FakeIntentRouter(),
        )
        return runtime, transport

    def valid_voice_messages(self, firmware):
        pcm = np.full(9000, 2000, dtype=np.int16)
        chunks = [
            firmware.audio_chunk(pcm[i : i + 256])
            for i in range(0, len(pcm), 256)
        ]
        return [firmware.voice_start(), *chunks, firmware.voice_end()]

    def deliver_valid_voice(self, runtime, firmware):
        firmware.deliver(runtime, *self.valid_voice_messages(firmware))

    def test_hello_from_firmware_is_recorded(self):
        runtime, firmware = self.make_runtime()

        firmware.deliver(runtime, firmware.firmware_hello())

        self.assertEqual(runtime.peer_capabilities["role"], "firmware")
        self.assertEqual(runtime.peer_capabilities["audio"]["format"], "pcm16")

    def test_wake_listen_speak_idle_sequence_without_hardware(self):
        runtime, firmware = self.make_runtime()
        pcm = np.full(9000, 2000, dtype=np.int16)
        chunks = [
            firmware.audio_chunk(pcm[i : i + 256])
            for i in range(0, len(pcm), 256)
        ]

        firmware.deliver(
            runtime,
            firmware.firmware_hello(),
            firmware.voice_start(),
            *chunks,
            firmware.voice_end(),
        )

        self.assertTrue(wait_for(lambda: runtime.state == "idle"))
        events = firmware.sent_session_events()
        self.assertEqual(events[0], SESSION_WAKE_DETECTED)
        self.assertEqual(events[1], SESSION_LISTEN_START)
        self.assertIn(SESSION_LISTEN_STOP, events)
        self.assertIn(SESSION_TTS_START, events)
        self.assertIn(SESSION_TTS_STOP, events)
        self.assertIn(SESSION_SESSION_DONE, events)
        self.assertIn(MSG_SAY, [msg_type for msg_type, _ in firmware.decoded_sent()])
        self.assertNotIn(SESSION_FOLLOWUP_ARM, firmware.sent_session_events())

    def test_wake_without_audio_returns_terminal_error(self):
        runtime, firmware = self.make_runtime()

        firmware.deliver(runtime, firmware.voice_start(), firmware.voice_end())

        self.assertTrue(wait_for(lambda: runtime.state == "idle"))
        events = firmware.sent_sessions()
        sent_types = [msg_type for msg_type, _ in firmware.decoded_sent()]
        self.assertEqual(events[0]["event"], SESSION_WAKE_DETECTED)
        self.assertEqual(events[1]["event"], SESSION_LISTEN_START)
        self.assertEqual(events[2]["event"], SESSION_LISTEN_STOP)
        self.assertEqual(events[-2]["event"], SESSION_SESSION_ERROR)
        self.assertEqual(events[-2]["reason"], "audio_rejected")
        self.assertEqual(events[-1]["event"], SESSION_SESSION_DONE)
        self.assertNotIn(MSG_SAY, sent_types)

    def test_audio_chunk_outside_voice_session_is_ignored(self):
        runtime, firmware = self.make_runtime()

        firmware.deliver(
            runtime,
            firmware.audio_chunk(np.full(9000, 2000, dtype=np.int16)),
            firmware.voice_end(),
        )

        self.assertTrue(wait_for(lambda: runtime.state == "idle"))
        self.assertEqual(firmware.sent_sessions(), [])
        self.assertEqual(firmware.decoded_sent(), [])

    def test_empty_session_does_not_contaminate_next_valid_session(self):
        runtime, firmware = self.make_runtime()
        pcm = np.full(9000, 2000, dtype=np.int16)
        chunks = [
            firmware.audio_chunk(pcm[i : i + 256])
            for i in range(0, len(pcm), 256)
        ]

        firmware.deliver(runtime, firmware.voice_start(), firmware.voice_end())
        self.assertTrue(wait_for(lambda: runtime.state == "idle"))
        firmware.deliver(runtime, firmware.voice_start(), *chunks, firmware.voice_end())

        self.assertTrue(wait_for(lambda: runtime.state == "idle"))
        sessions = firmware.sent_sessions()
        session_ids = {event["session_id"] for event in sessions}
        self.assertEqual(session_ids, {1, 2})
        self.assertEqual(firmware.sent_session_events().count(SESSION_SESSION_DONE), 2)
        self.assertEqual(
            [msg_type for msg_type, _ in firmware.decoded_sent()].count(MSG_SAY),
            1,
        )

    def test_voice_start_during_speaking_discards_previous_audio(self):
        runtime, firmware = self.make_runtime()
        old_audio = np.full(256, 1000, dtype=np.int16)
        new_audio = np.full(9000, 2000, dtype=np.int16)
        chunks = [
            firmware.audio_chunk(new_audio[i : i + 256])
            for i in range(0, len(new_audio), 256)
        ]

        firmware.deliver(
            runtime,
            firmware.voice_start(),
            firmware.audio_chunk(old_audio),
            firmware.voice_start(),
            *chunks,
            firmware.voice_end(),
        )

        self.assertTrue(wait_for(lambda: runtime.state == "idle"))
        events = firmware.sent_session_events()
        self.assertEqual(events.count(SESSION_WAKE_DETECTED), 2)
        self.assertIn(SESSION_SESSION_DONE, events)
        self.assertIn(MSG_SAY, [msg_type for msg_type, _ in firmware.decoded_sent()])

    def test_corrupt_frame_is_ignored_before_valid_session(self):
        runtime, firmware = self.make_runtime()

        firmware.deliver(
            runtime,
            firmware.corrupt_frame(),
            firmware.voice_start(),
            firmware.voice_end(),
        )

        self.assertTrue(wait_for(lambda: runtime.state == "idle"))
        events = firmware.sent_session_events()
        self.assertEqual(events[0], SESSION_WAKE_DETECTED)
        self.assertIn(SESSION_SESSION_DONE, events)

    def test_reconnect_starts_with_clean_runtime_state(self):
        first_runtime, first_firmware = self.make_runtime()
        self.deliver_valid_voice(first_runtime, first_firmware)
        self.assertTrue(wait_for(lambda: first_runtime.state == "idle"))

        second_runtime, second_firmware = self.make_runtime()
        second_firmware.deliver(second_runtime, second_firmware.firmware_hello())
        self.deliver_valid_voice(second_runtime, second_firmware)

        self.assertTrue(wait_for(lambda: second_runtime.state == "idle"))
        self.assertEqual(second_runtime.peer_capabilities["role"], "firmware")
        self.assertEqual({event["session_id"] for event in second_firmware.sent_sessions()}, {1})
        self.assertIn(MSG_SAY, [msg_type for msg_type, _ in second_firmware.decoded_sent()])

    def test_long_tts_reply_is_sent_as_multiple_say_chunks(self):
        runtime, firmware = self.make_runtime(tts=LongFakeTts())

        self.deliver_valid_voice(runtime, firmware)

        self.assertTrue(wait_for(lambda: runtime.state == "idle"))
        sent_types = [msg_type for msg_type, _ in firmware.decoded_sent()]
        self.assertEqual(sent_types.count(MSG_SAY), 6)
        events = firmware.sent_session_events()
        self.assertIn(SESSION_TTS_START, events)
        self.assertIn(SESSION_TTS_STOP, events)
        self.assertIn(SESSION_SESSION_DONE, events)

    def test_stt_rejection_reports_error_without_tts(self):
        runtime, firmware = self.make_runtime(stt=NoSpeechStt())

        self.deliver_valid_voice(runtime, firmware)

        self.assertTrue(wait_for(lambda: runtime.state == "idle"))
        sessions = firmware.sent_sessions()
        events = [event["event"] for event in sessions]
        session_errors = [
            event for event in sessions if event["event"] == SESSION_SESSION_ERROR
        ]
        self.assertNotIn(SESSION_TTS_START, events)
        self.assertEqual(session_errors[-1]["reason"], "stt_rejected")
        self.assertEqual(events[-1], SESSION_SESSION_DONE)

    def test_tts_failure_reports_error_and_sends_silent_ack(self):
        runtime, firmware = self.make_runtime(tts=FailingTts())

        self.deliver_valid_voice(runtime, firmware)

        self.assertTrue(wait_for(lambda: runtime.state == "idle"))
        frames = firmware.decoded_sent()
        sessions = firmware.sent_sessions()
        events = [event["event"] for event in sessions]
        session_errors = [
            event for event in sessions if event["event"] == SESSION_SESSION_ERROR
        ]
        say_payloads = [payload for msg_type, payload in frames if msg_type == MSG_SAY]
        self.assertIn(SESSION_TTS_START, events)
        self.assertIn(SESSION_TTS_STOP, events)
        self.assertEqual(session_errors[-1]["reason"], "tts_failed")
        self.assertEqual(say_payloads[-1], b"")


if __name__ == "__main__":
    unittest.main()
