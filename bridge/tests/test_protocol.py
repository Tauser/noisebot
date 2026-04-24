import unittest

from noisebot_bridge.protocol import (
    MSG_HELLO,
    PROTOCOL_VERSION,
    SESSION_LISTEN_START,
    decode_frames,
    decode_hello_payload,
    decode_session_payload,
    encode_frame,
    encode_hello_payload,
    encode_session_payload,
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

    def test_invalid_session_payload_is_rejected(self):
        with self.assertRaises(ValueError):
            decode_session_payload(b'{"event":"","session_id":1}')


if __name__ == "__main__":
    unittest.main()
