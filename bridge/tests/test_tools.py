import unittest

from noisebot_bridge.tools import RobotToolRuntime, TOOL_CATALOG, validate_tool_call


class ToolValidationTests(unittest.TestCase):
    def test_catalog_declares_required_robot_tools(self):
        for name in (
            "noisebot.robot.get_status",
            "noisebot.robot.set_gaze",
            "noisebot.robot.set_expression",
            "noisebot.robot.set_led_mood",
            "noisebot.robot.play_action",
            "noisebot.robot.create_reminder",
            "noisebot.robot.stop_reminder",
            "noisebot.robot.get_reminders",
        ):
            self.assertIn(name, TOOL_CATALOG)

    def test_ten_valid_commands_pass_schema(self):
        cases = (
            ("noisebot.robot.get_status", {}),
            ("noisebot.robot.set_gaze", {"direction": "esquerda"}),
            ("noisebot.robot.set_gaze", {"direction": "direita"}),
            ("noisebot.robot.set_gaze", {"direction": "cima"}),
            ("noisebot.robot.set_gaze", {"direction": "baixo"}),
            ("noisebot.robot.set_expression", {"expression_id": 1, "duration_ms": 4000}),
            ("noisebot.robot.play_action", {"action_id": 4}),
            ("noisebot.robot.emit_emotion_event", {"event_id": 2}),
            ("noisebot.robot.show_text", {"text": "Oi"}),
            ("noisebot.robot.create_reminder", {"text": "testar bridge", "due_at_epoch": 1800000000}),
        )

        for name, args in cases:
            with self.subTest(name=name):
                self.assertTrue(validate_tool_call(name, args).ok)

    def test_ten_invalid_commands_are_rejected_by_schema(self):
        cases = (
            ("noisebot.robot.unknown", {}, "unknown_tool"),
            ("noisebot.robot.set_led_mood", {"mood": "calmo"}, "unsupported_tool"),
            ("noisebot.robot.set_gaze", {}, "missing_arg:direction"),
            ("noisebot.robot.set_gaze", {"direction": "diagonal"}, "invalid_enum:direction"),
            ("noisebot.robot.set_expression", {"expression_id": -1}, "below_min:expression_id"),
            ("noisebot.robot.set_expression", {"expression_id": 8}, "above_max:expression_id"),
            ("noisebot.robot.play_action", {"action_id": 11}, "above_max:action_id"),
            ("noisebot.robot.show_text", {"text": "x" * 161}, "too_long:text"),
            ("noisebot.robot.create_reminder", {"text": "ok"}, "missing_arg:due_at_epoch"),
            ("noisebot.robot.stop_reminder", {"reminder_id": 0}, "below_min:reminder_id"),
        )

        for name, args, reason in cases:
            with self.subTest(name=name):
                result = validate_tool_call(name, args)
                self.assertFalse(result.ok)
                self.assertEqual(result.reason, reason)


class RobotToolRuntimeTests(unittest.TestCase):
    def test_status_tool_returns_last_bridge_status(self):
        runtime = RobotToolRuntime()
        runtime.update_status({"health": 99, "attention": 0.42})

        result = runtime.execute("noisebot.robot.get_status")

        self.assertTrue(result.ok)
        self.assertEqual(result.payload["status"]["health"], 99)
        self.assertEqual(result.payload["status"]["attention"], 0.42)

    def test_reminders_work_without_llm(self):
        runtime = RobotToolRuntime()

        created = runtime.execute(
            "noisebot.robot.create_reminder",
            {"text": "verificar bateria", "due_at_epoch": 1800000000},
        )
        listed = runtime.execute("noisebot.robot.get_reminders")
        stopped = runtime.execute(
            "noisebot.robot.stop_reminder",
            {"reminder_id": created.payload["reminder"]["id"]},
        )
        listed_after_stop = runtime.execute("noisebot.robot.get_reminders")

        self.assertTrue(created.ok)
        self.assertEqual(listed.payload["reminders"][0]["text"], "verificar bateria")
        self.assertTrue(stopped.ok)
        self.assertEqual(listed_after_stop.payload["reminders"], [])

    def test_firmware_tools_are_not_executed_by_local_runtime(self):
        runtime = RobotToolRuntime()

        result = runtime.execute("noisebot.robot.set_gaze", {"direction": "centro"})

        self.assertFalse(result.ok)
        self.assertEqual(result.error, "firmware_tool")


if __name__ == "__main__":
    unittest.main()
