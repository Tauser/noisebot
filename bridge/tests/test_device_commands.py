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
        self.assertEqual(result.name, "noisebot.robot.set_gaze")

    def test_canonical_gaze_tool_sends_msg_gaze(self):
        result = self.dispatcher.dispatch(
            DeviceCommand("noisebot.robot.set_gaze", {"direction": "direita"}, supported=True)
        )

        frames = decode_frames(bytearray(self.transport.sent[-1][1]))
        x, y = struct.unpack("<ff", frames[-1][1])
        self.assertTrue(result.executed)
        self.assertEqual(frames[-1][0], MSG_GAZE)
        self.assertGreater(x, 0.0)
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

    def test_invalid_gaze_direction_is_rejected_before_firmware(self):
        result = self.dispatcher.dispatch(DeviceCommand("look", {"direction": "diagonal"}, supported=True))

        self.assertTrue(result.supported)
        self.assertFalse(result.executed)
        self.assertEqual(result.error, "invalid_enum:direction")
        self.assertEqual(self.transport.sent, [])

    def test_expression_out_of_range_is_rejected_before_firmware(self):
        result = self.dispatcher.dispatch(
            DeviceCommand("set_expression", {"expression_id": 99, "duration_ms": 1200}, supported=True)
        )

        self.assertTrue(result.supported)
        self.assertFalse(result.executed)
        self.assertEqual(result.error, "above_max:expression_id")
        self.assertEqual(self.transport.sent, [])

    def test_future_led_tool_is_known_but_rejected_before_firmware(self):
        result = self.dispatcher.dispatch(
            DeviceCommand("noisebot.robot.set_led_mood", {"mood": "calmo"}, supported=True)
        )

        self.assertTrue(result.supported)
        self.assertFalse(result.executed)
        self.assertEqual(result.error, "unsupported_tool")
        self.assertEqual(self.transport.sent, [])

    def test_tool_logs_differentiate_call_result_and_rejected(self):
        with self.assertLogs("noisebot_bridge.device_commands", level="INFO") as cm:
            self.dispatcher.dispatch(DeviceCommand("look", {"direction": "centro"}, supported=True))
            self.dispatcher.dispatch(DeviceCommand("look", {"direction": "diagonal"}, supported=True))

        logs = "\n".join(cm.output)
        self.assertIn("tool_call name=noisebot.robot.set_gaze", logs)
        self.assertIn("tool_result name=noisebot.robot.set_gaze status=ok", logs)
        self.assertIn("tool_rejected name=noisebot.robot.set_gaze reason=invalid_enum:direction", logs)


if __name__ == "__main__":
    unittest.main()
