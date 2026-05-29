import unittest

from noisebot_bridge.protocol import (
    BRIDGE_HELLO_CAPABILITIES,
    MSG_HELLO,
    PROTOCOL_VERSION,
    SESSION_ABORT_SPEAKING,
    SESSION_FOLLOWUP_ARM,
    SESSION_LISTEN_START,
    SESSION_LISTEN_STOP,
    SESSION_SESSION_DONE,
    SESSION_SPEAK_START,
    SESSION_SPEAK_STOP,
    SESSION_THINKING_START,
    SESSION_TRANSCRIBE_START,
    SESSION_TTS_START,
    SESSION_TTS_STOP,
    SESSION_WAKE_DETECTED,
    decode_frames,
    decode_hello_payload,
    decode_session_payload,
    encode_frame,
    encode_hello_payload,
    encode_session_payload,
    validate_pcm16_audio_contract,
)


class ProtocolTests(unittest.TestCase):
    def test_round_trip_empty_frame(self):
        buf = bytearray(encode_frame(MSG_HELLO))
        self.assertEqual(decode_frames(buf), [(MSG_HELLO, b"")])
        self.assertEqual(buf, bytearray())

    def test_round_trip_payload_frame(self):
        payload = b"abc123"
        buf = bytearray(encode_frame(0x42, payload))
        self.assertEqual(decode_frames(buf), [(0x42, payload)])

    def test_partial_frame_waits_for_more_bytes(self):
        frame = encode_frame(0x42, b"abc")
        buf = bytearray(frame[:-1])
        self.assertEqual(decode_frames(buf), [])
        self.assertEqual(buf, bytearray(frame[:-1]))

    def test_bad_crc_discards_frame(self):
        frame = bytearray(encode_frame(0x42, b"abc"))
        frame[-1] ^= 0xFF
        buf = bytearray(frame)
        self.assertEqual(decode_frames(buf), [])
        self.assertEqual(buf, bytearray())

    def test_hello_payload_v2_round_trip(self):
        payload = encode_hello_payload()
        decoded = decode_hello_payload(payload)
        self.assertEqual(decoded["protocol"], "noisebot-bridge")
        self.assertEqual(decoded["version"], PROTOCOL_VERSION)
        self.assertEqual(decoded["role"], "bridge")
        self.assertIn("audio_chunk", decoded["rx"])
        self.assertIn("volume", decoded["tx"])

    def test_hello_capabilities_truthful(self):
        decoded = decode_hello_payload(encode_hello_payload())

        self.assertEqual(decoded["audio"]["format"], "pcm16")
        self.assertEqual(decoded["audio"]["sample_rate"], 16000)
        self.assertEqual(decoded["audio"]["channels"], 1)
        self.assertEqual(decoded["audio"]["chunk_samples"], 256)
        self.assertIn("speech_cancel", decoded["tx"])
        self.assertEqual(decoded["codecs"], {"pcm16": True, "opus": False})
        self.assertTrue(decoded["conversation"]["auto"])
        self.assertFalse(decoded["conversation"]["manual"])
        self.assertFalse(decoded["conversation"]["followup"])
        self.assertFalse(decoded["conversation"]["realtime"])
        self.assertTrue(decoded["audio_processor"]["afe_opt_in"])
        self.assertFalse(decoded["audio_processor"]["afe_default"])
        self.assertFalse(decoded["audio_processor"]["aec_supported"])
        self.assertFalse(decoded["audio_processor"]["device_aec"])

    def test_protocol_keeps_pcm16_default(self):
        caps = BRIDGE_HELLO_CAPABILITIES

        self.assertEqual(caps["audio"]["format"], "pcm16")
        self.assertTrue(caps["codecs"]["pcm16"])
        self.assertFalse(caps["codecs"]["opus"])
        validate_pcm16_audio_contract(caps)

    def test_audio_contract_rejects_opus_until_enabled(self):
        caps = {
            **BRIDGE_HELLO_CAPABILITIES,
            "codecs": {"pcm16": True, "opus": True},
        }

        with self.assertRaises(ValueError):
            validate_pcm16_audio_contract(caps)

    def test_audio_contract_rejects_chunk_size_change(self):
        caps = {
            **BRIDGE_HELLO_CAPABILITIES,
            "audio": {**BRIDGE_HELLO_CAPABILITIES["audio"], "chunk_samples": 960},
        }

        with self.assertRaises(ValueError):
            validate_pcm16_audio_contract(caps)

    def test_aec_realtime_opus_not_advertised_when_disabled(self):
        decoded = decode_hello_payload(encode_hello_payload())

        self.assertFalse(decoded["codecs"]["opus"])
        self.assertFalse(decoded["conversation"]["realtime"])
        self.assertFalse(decoded["audio_processor"]["aec_supported"])
        self.assertFalse(decoded["audio_processor"]["device_aec"])

    def test_followup_arm_ignored_when_feature_off(self):
        decoded = decode_hello_payload(encode_hello_payload())
        payload = encode_session_payload(SESSION_FOLLOWUP_ARM, 8, window_ms=8000)
        event = decode_session_payload(payload)

        self.assertFalse(decoded["conversation"]["followup"])
        self.assertEqual(event["event"], SESSION_FOLLOWUP_ARM)
        self.assertEqual(event["window_ms"], 8000)

    def test_empty_hello_payload_is_v1_compatible(self):
        decoded = decode_hello_payload(b"")
        self.assertEqual(decoded["version"], 1)
        self.assertEqual(decoded["role"], "unknown")

    def test_invalid_hello_payload_is_rejected(self):
        with self.assertRaises(ValueError):
            decode_hello_payload(b"{")

    def test_session_payload_v2_round_trip(self):
        payload = encode_session_payload(SESSION_LISTEN_START, 7, source="wake_word")
        decoded = decode_session_payload(payload)
        self.assertEqual(decoded["event"], SESSION_LISTEN_START)
        self.assertEqual(decoded["session_id"], 7)
        self.assertEqual(decoded["source"], "wake_word")

    def test_conversation_v2_event_sequence_round_trip(self):
        sequence = [
            SESSION_WAKE_DETECTED,
            SESSION_LISTEN_START,
            SESSION_LISTEN_STOP,
            SESSION_TRANSCRIBE_START,
            SESSION_THINKING_START,
            SESSION_TTS_START,
            SESSION_SPEAK_START,
            SESSION_SPEAK_STOP,
            SESSION_TTS_STOP,
            SESSION_SESSION_DONE,
        ]

        decoded = [
            decode_session_payload(encode_session_payload(event, 9, source="test"))["event"]
            for event in sequence
        ]

        self.assertEqual(decoded, sequence)

    def test_abort_speaking_event_round_trip(self):
        payload = encode_session_payload(SESSION_ABORT_SPEAKING, 10, reason="wake_word_detected")
        decoded = decode_session_payload(payload)

        self.assertEqual(decoded["event"], SESSION_ABORT_SPEAKING)
        self.assertEqual(decoded["reason"], "wake_word_detected")

    def test_invalid_session_payload_is_rejected(self):
        with self.assertRaises(ValueError):
            decode_session_payload(b'{"event":"","session_id":1}')


if __name__ == "__main__":
    unittest.main()
