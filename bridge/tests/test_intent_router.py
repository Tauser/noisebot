import unittest
from datetime import datetime

from noisebot_bridge.intent_router import LocalIntentRouter, normalize_text
from noisebot_bridge.market import MarketPrice
from noisebot_bridge.weather import WeatherNow


class IntentRouterTests(unittest.TestCase):
    def setUp(self):
        self.router = LocalIntentRouter()

    def test_normalize_pt_br_text(self):
        self.assertEqual(normalize_text("Que horas são agora?"), "que horas sao agora")

    def test_time_intent_accepts_clean_phrase(self):
        result = self.router.route("Que horas são agora?", now=datetime(2026, 4, 23, 8, 5))
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_time")
        self.assertEqual(result.reply, "Agora são 8 horas e 05 minutos. São Paulo, 23/04/2026.")
        self.assertTrue(result.speak_reply)

    def test_time_intent_handles_one_hour_singular(self):
        result = self.router.route("Que horas são?", now=datetime(2026, 4, 23, 1, 17))
        self.assertIsNotNone(result)
        self.assertEqual(result.reply, "Agora é 1 hora e 17 minutos. São Paulo, 23/04/2026.")

    def test_time_intent_handles_one_minute_singular(self):
        result = self.router.route("Que horas são?", now=datetime(2026, 4, 23, 2, 1))
        self.assertIsNotNone(result)
        self.assertEqual(result.reply, "Agora são 2 horas e 01 minuto. São Paulo, 23/04/2026.")

    def test_time_intent_handles_midnight(self):
        result = self.router.route("Que horas são?", now=datetime(2026, 4, 23, 0, 0))
        self.assertIsNotNone(result)
        self.assertEqual(result.reply, "Agora é meia-noite. São Paulo, 23/04/2026.")

    def test_time_intent_accepts_whisper_artifact(self):
        result = self.router.route("E horas são agora.", now=datetime(2026, 4, 23, 23, 0))
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_time")
        self.assertEqual(result.reply, "Agora são 23 horas. São Paulo, 23/04/2026.")

    def test_date_intent_answers_today(self):
        result = self.router.route("Que dia é hoje?", now=datetime(2026, 4, 23, 8, 5))

        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_date")
        self.assertEqual(result.reply, "Hoje é quinta-feira, 23 de abril de 2026.")

    def test_status_intent_uses_status_fields(self):
        result = self.router.route("qual seu status", status={"health": 99, "attention": 0.42})
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_status")
        self.assertEqual(result.reply, "Status: saude 99%, atencao 42%.")
        self.assertTrue(result.speak_reply)
        self.assertEqual(result.device_commands[0].name, "show_status")
        self.assertTrue(result.device_commands[0].supported)

    def test_status_intent_accepts_show_status_command(self):
        result = self.router.route("mostrar status")

        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_status")
        self.assertEqual(result.reply, "")
        self.assertFalse(result.speak_reply)
        self.assertEqual(result.device_commands[0].name, "show_status")
        self.assertTrue(result.device_commands[0].supported)

    def test_status_intent_accepts_show_the_status_command(self):
        result = self.router.route("mostra o status")

        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_status")
        self.assertEqual(result.reply, "")
        self.assertFalse(result.speak_reply)
        self.assertEqual(result.device_commands[0].name, "show_status")
        self.assertTrue(result.device_commands[0].supported)

    def test_status_intent_accepts_display_status_command_without_text(self):
        result = self.router.route("exibir status")

        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_status")
        self.assertEqual(result.reply, "")
        self.assertFalse(result.speak_reply)
        self.assertEqual(result.device_commands[0].name, "show_status")
        self.assertTrue(result.device_commands[0].supported)

    def test_status_intent_accepts_exiba_status_command_without_text(self):
        result = self.router.route("exiba status")

        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_status")
        self.assertEqual(result.reply, "")
        self.assertFalse(result.speak_reply)
        self.assertEqual(result.device_commands[0].name, "show_status")
        self.assertTrue(result.device_commands[0].supported)

    def test_status_intent_accepts_transcribed_wrong_article_without_text(self):
        result = self.router.route("Mostre a status.")

        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_status")
        self.assertEqual(result.reply, "")
        self.assertFalse(result.speak_reply)
        self.assertEqual(result.device_commands[0].name, "show_status")
        self.assertTrue(result.device_commands[0].supported)

    def test_status_intent_accepts_transcribed_ai_particle_without_text(self):
        result = self.router.route("Mostra aí status.")

        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_status")
        self.assertEqual(result.reply, "")
        self.assertFalse(result.speak_reply)
        self.assertEqual(result.device_commands[0].name, "show_status")
        self.assertTrue(result.device_commands[0].supported)

    def test_social_wellbeing_question_uses_mood_not_status(self):
        result = self.router.route("como você está", status={"health": 99, "attention": 0.42})

        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_mood")
        self.assertNotIn("Status:", result.reply)
        self.assertNotIn("operacional", result.reply)
        self.assertTrue(result.speak_reply)

    def test_social_wellbeing_reply_reflects_low_valence(self):
        result = self.router.route("como você está", status={"valence": -0.6, "activation": 0.4, "attention": 0.6})

        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_mood")
        self.assertIn("atravessado", result.reply)
        self.assertEqual(result.expression_id, 7)

    def test_social_wellbeing_reply_reflects_high_activation(self):
        result = self.router.route("tudo bem", status={"valence": 0.1, "activation": 0.8, "attention": 0.6})

        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_mood")
        self.assertIn("acordado", result.reply)
        self.assertEqual(result.expression_id, 1)

    def test_bridge_test_intent(self):
        result = self.router.route("você está me ouvindo?")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_bridge_test")
        self.assertEqual(result.reply, "Bridge: ouvindo.")
        self.assertTrue(result.speak_reply)

    def test_network_status_intent(self):
        result = self.router.route("qual seu ip")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_network_status")
        self.assertEqual(result.reply, "Rede: bridge conectado, ip indisponivel.")
        self.assertTrue(result.speak_reply)
        self.assertEqual(result.device_commands[0].name, "show_status")
        self.assertTrue(result.device_commands[0].supported)

    def test_network_status_uses_ip_when_available(self):
        result = self.router.route("qual seu ip", status={"ip": "192.168.1.23"})

        self.assertIsNotNone(result)
        self.assertEqual(result.reply, "Rede: bridge conectado, ip 192.168.1.23.")
        self.assertTrue(result.speak_reply)

    def test_volume_intent_clamps_percent(self):
        result = self.router.route("coloque o volume em 150%")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_device_volume")
        self.assertEqual(result.device_commands[0].args["percent"], 100)
        self.assertTrue(result.device_commands[0].supported)
        self.assertTrue(result.speak_reply)

    def test_volume_intent_relative_up_uses_status_volume(self):
        result = self.router.route("aumentar volume", status={"volume": 55})

        self.assertIsNotNone(result)
        self.assertEqual(result.device_commands[0].args["percent"], 65)
        self.assertTrue(result.device_commands[0].supported)
        self.assertTrue(result.speak_reply)

    def test_volume_intent_accepts_som_alias(self):
        result = self.router.route("aumente o som", status={"volume": 40})

        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_device_volume")
        self.assertEqual(result.device_commands[0].args["percent"], 50)
        self.assertTrue(result.speak_reply)

    def test_volume_intent_accepts_loudness_without_volume_word(self):
        result = self.router.route("fala mais alto", status={"volume": 40})

        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_device_volume")
        self.assertEqual(result.device_commands[0].args["percent"], 50)

    def test_movement_intent_maps_to_supported_gaze_command(self):
        result = self.router.route("olhe para esquerda")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_device_move")
        self.assertEqual(result.device_commands[0].args["direction"], "esquerda")
        self.assertTrue(result.device_commands[0].supported)
        self.assertTrue(result.speak_reply)

    def test_expression_intent_maps_to_supported_expression_command(self):
        result = self.router.route("fique feliz")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_device_expression")
        self.assertEqual(result.device_commands[0].name, "set_expression")
        self.assertEqual(result.device_commands[0].args["expression_id"], 1)
        self.assertTrue(result.device_commands[0].supported)
        self.assertTrue(result.speak_reply)

    def test_expression_intent_accepts_short_keyword_command(self):
        result = self.router.route("feliz")

        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_device_expression")
        self.assertEqual(result.device_commands[0].args["expression_id"], 1)

    def test_expression_intent_accepts_common_synonym(self):
        result = self.router.route("sorria")

        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_device_expression")
        self.assertEqual(result.device_commands[0].args["expression_id"], 1)

    def test_action_intent_maps_to_supported_action_command(self):
        result = self.router.route("balance a cabeça")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_device_action")
        self.assertEqual(result.device_commands[0].name, "play_action")
        self.assertTrue(result.device_commands[0].supported)
        self.assertTrue(result.speak_reply)

    def test_action_intent_accepts_whisper_head_action_artifact(self):
        result = self.router.route("Balão se acabece.")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_device_action")
        self.assertEqual(result.device_commands[0].name, "play_action")
        self.assertTrue(result.device_commands[0].supported)

    def test_bitcoin_price_intent_uses_local_market_tool(self):
        import noisebot_bridge.intent_router as intent_router

        original_fetch = intent_router.fetch_btc_price
        try:
            intent_router.fetch_btc_price = lambda: MarketPrice(asset="BTC", usd=100000.0, brl=550000.0, source="Teste")
            result = self.router.route("qual o valor do bitcoin nesse momento")
        finally:
            intent_router.fetch_btc_price = original_fetch

        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_market_btc_price")
        self.assertIn("US$ 100.000,00", result.reply)
        self.assertIn("R$ 550.000,00", result.reply)

    def test_weather_intent_uses_local_weather_tool(self):
        import noisebot_bridge.intent_router as intent_router

        original_fetch = intent_router.fetch_weather_now
        try:
            intent_router.fetch_weather_now = lambda: WeatherNow(
                temperature_c=24.4,
                weather_code=2,
                location="Brasília",
            )
            result = self.router.route("qual a temperatura atual")
        finally:
            intent_router.fetch_weather_now = original_fetch

        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_weather")
        self.assertEqual(result.reply, "Agora em Brasília está 24 graus, com parcialmente nublado.")

    def test_weather_intent_accepts_climate_phrase(self):
        import noisebot_bridge.intent_router as intent_router

        original_fetch = intent_router.fetch_weather_now
        try:
            intent_router.fetch_weather_now = lambda: WeatherNow(
                temperature_c=19.8,
                weather_code=61,
                location="São Paulo",
            )
            result = self.router.route("como está o clima hoje")
        finally:
            intent_router.fetch_weather_now = original_fetch

        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_weather")
        self.assertIn("20 graus", result.reply)

    def test_unknown_text_returns_none(self):
        self.assertIsNone(self.router.route("fale sobre isaac newton"))

    # ── ANGRY — expressão explícita ───────────────────────────────────────────

    def test_angry_expression_fica_bravo(self):
        result = self.router.route("fica bravo")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_device_expression_angry")
        self.assertEqual(result.expression_id, 9)
        self.assertTrue(result.speak_reply)
        self.assertTrue(result.device_commands[0].supported)
        self.assertEqual(result.device_commands[0].args["expression_id"], 9)

    def test_angry_expression_cara_de_bravo(self):
        result = self.router.route("cara de bravo")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_device_expression_angry")
        self.assertEqual(result.expression_id, 9)

    def test_angry_expression_modo_raiva(self):
        result = self.router.route("modo raiva")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_device_expression_angry")
        self.assertEqual(result.expression_id, 9)

    def test_angry_expression_fique_irritado(self):
        result = self.router.route("fique irritado")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_device_expression_angry")
        self.assertEqual(result.expression_id, 9)

    def test_angry_expression_expressao_brava(self):
        result = self.router.route("expressão brava")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_device_expression_angry")
        self.assertEqual(result.expression_id, 9)

    def test_angry_expression_play_duration_is_3000ms(self):
        result = self.router.route("fica bravo")
        self.assertIsNotNone(result)
        self.assertEqual(result.device_commands[0].args["duration_ms"], 3000)

    # ── ANGRY — provocações ───────────────────────────────────────────────────

    def test_angry_provocation_robo_burro(self):
        result = self.router.route("robô burro")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_angry_provocation")
        self.assertEqual(result.expression_id, 9)
        self.assertTrue(result.speak_reply)
        self.assertNotEqual(result.reply, "")

    def test_angry_provocation_voce_errou(self):
        result = self.router.route("você errou")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_angry_provocation")
        self.assertEqual(result.expression_id, 9)

    def test_angry_provocation_prefiro_chatgpt(self):
        result = self.router.route("prefiro o chatgpt")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_angry_provocation")
        self.assertEqual(result.expression_id, 9)

    def test_angry_provocation_reply_is_theatrical(self):
        """Verifica que a resposta é uma das frases teatrais pré-definidas."""
        from noisebot_bridge.intent_router import LocalIntentRouter
        result = self.router.route("robô idiota")
        self.assertIsNotNone(result)
        self.assertIn(result.reply, LocalIntentRouter._ANGRY_REPLIES)

    def test_angry_provocation_has_no_device_commands(self):
        result = self.router.route("você errou de novo")
        self.assertIsNotNone(result)
        self.assertEqual(result.intent, "local_angry_provocation")
        self.assertEqual(len(result.device_commands), 0)

    # ── ANGRY — garantias de não-baseline ────────────────────────────────────

    def test_angry_expression_does_not_suppress_speak(self):
        """ANGRY via intent deve falar a resposta teatral (speak_reply=True)."""
        result = self.router.route("fica bravo")
        self.assertIsNotNone(result)
        self.assertTrue(result.speak_reply)

    def test_non_angry_expression_speaks_confirmation(self):
        """Expressões locais também falam confirmação ao usuário."""
        result = self.router.route("fique feliz")
        self.assertIsNotNone(result)
        self.assertTrue(result.speak_reply)


if __name__ == "__main__":
    unittest.main()

