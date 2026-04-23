import unittest
from datetime import datetime

from noisebot_bridge.intent_router import LocalIntentRouter, normalize_text


class IntentRouterTests(unittest.TestCase):
    def setUp(self):
        self.router = LocalIntentRouter()

    def test_normalize_pt_br_text(self):
        self.assertEqual(normalize_text("Que horas são agora?"), "que horas sao agora")

    def test_time_intent_accepts_clean_phrase(self):
        result = self.router.route("Que horas são agora?", now=datetime(2026, 4, 23, 8, 5))
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_time")
        self.assertEqual(result.reply, "Agora são 8 horas e 05 minutos.")

    def test_time_intent_handles_one_hour_singular(self):
        result = self.router.route("Que horas são?", now=datetime(2026, 4, 23, 1, 17))
        self.assertIsNotNone(result)
        self.assertEqual(result.reply, "Agora é 1 hora e 17 minutos.")

    def test_time_intent_handles_one_minute_singular(self):
        result = self.router.route("Que horas são?", now=datetime(2026, 4, 23, 2, 1))
        self.assertIsNotNone(result)
        self.assertEqual(result.reply, "Agora são 2 horas e 01 minuto.")

    def test_time_intent_handles_midnight(self):
        result = self.router.route("Que horas são?", now=datetime(2026, 4, 23, 0, 0))
        self.assertIsNotNone(result)
        self.assertEqual(result.reply, "Agora é meia-noite.")

    def test_time_intent_accepts_whisper_artifact(self):
        result = self.router.route("E horas são agora.", now=datetime(2026, 4, 23, 23, 0))
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_time")
        self.assertEqual(result.reply, "Agora são 23 horas.")

    def test_status_intent_uses_status_fields(self):
        result = self.router.route("qual seu status", status={"health": 99, "attention": 0.42})
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_status")
        self.assertIn("saúde 99 de 100", result.reply)

    def test_bridge_test_intent(self):
        result = self.router.route("você está me ouvindo?")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_bridge_test")

    def test_network_status_intent(self):
        result = self.router.route("qual seu ip")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_network_status")

    def test_volume_intent_clamps_percent(self):
        result = self.router.route("coloque o volume em 150%")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_device_volume")
        self.assertEqual(result.device_commands[0].args["percent"], 100)
        self.assertFalse(result.device_commands[0].supported)

    def test_movement_intent_maps_to_supported_gaze_command(self):
        result = self.router.route("olhe para esquerda")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_device_move")
        self.assertEqual(result.device_commands[0].args["direction"], "esquerda")
        self.assertTrue(result.device_commands[0].supported)

    def test_expression_intent_maps_to_supported_expression_command(self):
        result = self.router.route("fique feliz")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_device_expression")
        self.assertEqual(result.device_commands[0].name, "set_expression")
        self.assertEqual(result.device_commands[0].args["expression_id"], 1)
        self.assertTrue(result.device_commands[0].supported)

    def test_action_intent_maps_to_supported_action_command(self):
        result = self.router.route("balance a cabeça")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_device_action")
        self.assertEqual(result.device_commands[0].name, "play_action")
        self.assertTrue(result.device_commands[0].supported)

    def test_action_intent_accepts_whisper_head_action_artifact(self):
        result = self.router.route("Balão se acabece.")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_device_action")
        self.assertEqual(result.device_commands[0].name, "play_action")
        self.assertTrue(result.device_commands[0].supported)

    def test_unknown_text_returns_none(self):
        self.assertIsNone(self.router.route("fale sobre isaac newton"))


if __name__ == "__main__":
    unittest.main()
