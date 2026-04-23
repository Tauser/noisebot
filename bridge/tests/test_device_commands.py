import struct
import unittest

from noisebot_bridge.device_commands import DeviceCommandDispatcher
from noisebot_bridge.intent_router import DeviceCommand
from noisebot_bridge.protocol import MSG_ACTION, MSG_EXPR, MSG_GAZE, decode_frames, encode_frame
from noisebot_bridge.transport import NullTransport


class DeviceCommandDispatcherTests(unittest.TestCase):
    def setUp(self):
        self.transport = NullTransport()
        self.dispatcher = DeviceCommandDispatcher(lambda msg_type, payload=b"": self.transport.send(encode_frame(msg_type, payload)))

    def test_supported_gaze_command_sends_msg_gaze(self):
        result = self.dispatcher.dispatch(DeviceCommand("look", {"direction": "esquerda"}, supported=True))

        frames = decode_frames(bytearray(self.transport.sent[-1][1]))
        x, y = struct.unpack("<ff", frames[-1][1])
        self.assertTrue(result.executed)
        self.assertEqual(frames[-1][0], MSG_GAZE)
        self.assertLess(x, 0.0)
        self.assertEqual(y, 0.0)

    def test_supported_expression_command_sends_msg_expr(self):
        result = self.dispatcher.dispatch(
            DeviceCommand("set_expression", {"expression_id": 1, "duration_ms": 1200}, supported=True)
        )

        frames = decode_frames(bytearray(self.transport.sent[-1][1]))
        expression_id, duration_ms = struct.unpack("<BI", frames[-1][1])
        self.assertTrue(result.executed)
        self.assertEqual(frames[-1][0], MSG_EXPR)
        self.assertEqual(expression_id, 1)
        self.assertEqual(duration_ms, 1200)

    def test_supported_action_command_sends_msg_action(self):
        result = self.dispatcher.dispatch(DeviceCommand("play_action", {"action_id": 4}, supported=True))

        frames = decode_frames(bytearray(self.transport.sent[-1][1]))
        action_id, = struct.unpack("<I", frames[-1][1])
        self.assertTrue(result.executed)
        self.assertEqual(frames[-1][0], MSG_ACTION)
        self.assertEqual(action_id, 4)

    def test_unsupported_command_is_logged_but_not_sent(self):
        result = self.dispatcher.dispatch(DeviceCommand("set_led_color", {"color": "azul"}, supported=False))

        self.assertFalse(result.supported)
        self.assertFalse(result.executed)
        self.assertEqual(self.transport.sent, [])


if __name__ == "__main__":
    unittest.main()
