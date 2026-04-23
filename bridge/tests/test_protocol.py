import unittest

from noisebot_bridge.protocol import MSG_HELLO, decode_frames, encode_frame


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


if __name__ == "__main__":
    unittest.main()
