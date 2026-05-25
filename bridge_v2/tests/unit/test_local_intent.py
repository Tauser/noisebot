"""Testes unitários: LocalIntentProvider — intents PT-BR determinísticos.

Cobre:
  - Todos os intents definidos (local_time, local_bridge_test, local_status,
    local_network_status, local_market_btc_price, local_mood, local_greeting,
    local_farewell, local_look_up/down/left/right, local_volume_up/down)
  - Normalização de texto (acentos, maiúsculas, pontuação)
  - Texto vazio → intent_name=None
  - Texto sem match → intent_name=None
  - reply_text preenchido corretamente
  - expression_id / action_id / emot_event_id populados
  - Variações de phrasing (diferentes formas de dizer a mesma coisa)
  - turn_id propagado corretamente
  - local_time usa `now` injetado
"""
from __future__ import annotations

from datetime import datetime

import pytest

from bridgev2.llm.local_intent import LocalIntentProvider
from bridgev2.runtime.events import IntentResolved
from bridgev2.vision import FaceBox, VisionAnalysis, VisionError, VisionObservation


@pytest.fixture()
def provider() -> LocalIntentProvider:
    return LocalIntentProvider()


class FakeVisionClient:
    def __init__(
        self,
        observation: VisionObservation | None = None,
        error: Exception | None = None,
        analysis: VisionAnalysis | None = None,
    ) -> None:
        self.observation = observation or VisionObservation(
            valid=True,
            scene="normal",
            timestamp_ms=123,
            width=640,
            height=480,
            jpeg_bytes=54000,
            capture_ms=900,
            luma_avg=122,
            luma_min=0,
            luma_max=255,
            contrast=255,
            motion_score=5,
        )
        self.error = error
        self.analysis = analysis
        self.calls = 0

    def observe(self) -> VisionObservation:
        self.calls += 1
        if self.error:
            raise self.error
        return self.observation

    def analyze(self) -> VisionAnalysis:
        self.calls += 1
        if self.error:
            raise self.error
        return self.analysis or VisionAnalysis(
            observation=self.observation,
            detector="test",
            detector_available=False,
            face_detected=False,
            face_count=0,
        )


def match(provider: LocalIntentProvider, text: str, turn_id: int = 1, **kwargs) -> IntentResolved:
    return provider.match(text=text, turn_id=turn_id, **kwargs)


# ── Sem match ─────────────────────────────────────────────────────────────────

class TestNoMatch:
    def test_empty_string_returns_none_intent(self, provider):
        result = match(provider, "")
        assert result.intent_name is None
        assert result.has_intent is False

    def test_whitespace_only_returns_none_intent(self, provider):
        result = match(provider, "   ")
        assert result.intent_name is None

    def test_unknown_phrase_returns_none_intent(self, provider):
        result = match(provider, "qual a receita de bolo de cenoura")
        assert result.intent_name is None

    def test_turn_id_propagated_on_no_match(self, provider):
        result = match(provider, "", turn_id=42)
        assert result.turn_id == 42


# ── local_time ────────────────────────────────────────────────────────────────

class TestLocalTime:
    _phrases = [
        "que horas são",
        "Que Horas São?",
        "me diz a hora",
        "horas agora",
        "que hora é",
    ]

    @pytest.mark.parametrize("text", _phrases)
    def test_phrases_match(self, provider, text):
        result = match(provider, text, now=datetime(2024, 6, 15, 10, 30))
        assert result.intent_name == "local_time"

    def test_reply_text_on_hour(self, provider):
        result = match(provider, "que horas são", now=datetime(2024, 1, 1, 14, 0))
        assert "14" in result.reply_text
        assert "horas" in result.reply_text

    def test_reply_text_with_minutes(self, provider):
        result = match(provider, "que horas são", now=datetime(2024, 1, 1, 9, 35))
        assert "9" in result.reply_text
        assert "35" in result.reply_text

    def test_reply_midnight(self, provider):
        result = match(provider, "que horas são", now=datetime(2024, 1, 1, 0, 0))
        assert "meia-noite" in result.reply_text.lower()

    def test_reply_one_hour(self, provider):
        result = match(provider, "que horas são", now=datetime(2024, 1, 1, 1, 0))
        assert "hora" in result.reply_text  # "1 hora" (singular)
        assert "horas" not in result.reply_text

    def test_expression_and_action_set(self, provider):
        result = match(provider, "que horas são", now=datetime(2024, 1, 1, 10, 0))
        assert result.expression_id is not None
        assert result.action_id is not None
        assert result.emot_event_id is not None

    def test_turn_id_propagated(self, provider):
        result = match(provider, "que horas são", turn_id=99, now=datetime(2024, 1, 1, 10, 0))
        assert result.turn_id == 99


# ── local_date ────────────────────────────────────────────────────────────────

class TestLocalDate:
    def test_today_date(self, provider):
        result = match(provider, "que dia é hoje", now=datetime(2026, 4, 23, 10, 30))

        assert result.intent_name == "local_date"
        assert result.reply_text == "Hoje e quinta-feira, 23 de abril de 2026."


# ── local_bridge_test ─────────────────────────────────────────────────────────

class TestLocalBridgeTest:
    _phrases = [
        "teste do bridge",
        "bridge funcionando",
        "bridge ok",
        "bridge teste",
    ]

    @pytest.mark.parametrize("text", _phrases)
    def test_phrases_match(self, provider, text):
        result = match(provider, text)
        assert result.intent_name == "local_bridge_test"

    def test_reply_contains_bridge(self, provider):
        result = match(provider, "bridge ok")
        assert "Bridge" in result.reply_text or "bridge" in result.reply_text.lower()


# ── local_status ──────────────────────────────────────────────────────────────

class TestLocalStatus:
    _phrases = [
        "seu status",
        "status do sistema",
        "diagnóstico",
    ]

    @pytest.mark.parametrize("text", _phrases)
    def test_phrases_match(self, provider, text):
        result = match(provider, text)
        assert result.intent_name == "local_status"

    def test_reply_with_health_and_attention(self, provider):
        ctx = {"status": {"health": 85, "attention": 0.72}}
        result = match(provider, "status do sistema", context=ctx)
        assert "85" in result.reply_text
        assert "72" in result.reply_text

    def test_reply_without_status_context(self, provider):
        result = match(provider, "status do sistema", context={})
        assert "operacional" in result.reply_text.lower()


# ── local_network_status ──────────────────────────────────────────────────────

class TestLocalNetworkStatus:
    _phrases = [
        "está conectado",
        "tem conexão",
        "tem internet",
        "status da rede",
        "rede ok",
    ]

    @pytest.mark.parametrize("text", _phrases)
    def test_phrases_match(self, provider, text):
        result = match(provider, text)
        assert result.intent_name == "local_network_status"

    def test_reply_with_ip(self, provider):
        ctx = {"status": {"ip": "192.168.1.10"}}
        result = match(provider, "status da rede", context=ctx)
        assert "192.168.1.10" in result.reply_text

    def test_reply_without_ip(self, provider):
        result = match(provider, "rede ok", context={"status": {}})
        assert "indisponivel" in result.reply_text.lower()


# ── local_market_btc_price ────────────────────────────────────────────────────

class TestLocalBtcPrice:
    _phrases = [
        "bitcoin",
        "BTC",
        "preço do bitcoin",
        "quanto vale",
        "criptomoeda",
    ]

    @pytest.mark.parametrize("text", _phrases)
    def test_phrases_match(self, provider, text):
        result = match(provider, text)
        assert result.intent_name == "local_market_btc_price"

    def test_reply_is_placeholder(self, provider):
        result = match(provider, "bitcoin")
        # Sem fetch de rede — deve indicar indisponibilidade
        assert result.reply_text is not None
        assert len(result.reply_text) > 0


# ── local_weather ─────────────────────────────────────────────────────────────

class TestLocalWeather:
    def test_temperature_intent_uses_weather_provider(self, provider, monkeypatch):
        import bridgev2.llm.local_intent as local_intent
        from bridgev2.llm.weather import WeatherNow

        monkeypatch.setattr(
            local_intent,
            "fetch_weather_now",
            lambda: WeatherNow(temperature_c=24.4, weather_code=2, location="Brasilia"),
        )

        result = match(provider, "qual a temperatura atual")

        assert result.intent_name == "local_weather"
        assert result.reply_text == "Agora em Brasilia esta 24 graus, com parcialmente nublado."

    def test_climate_phrase_uses_weather_provider(self, provider, monkeypatch):
        import bridgev2.llm.local_intent as local_intent
        from bridgev2.llm.weather import WeatherNow

        monkeypatch.setattr(
            local_intent,
            "fetch_weather_now",
            lambda: WeatherNow(temperature_c=19.8, weather_code=61, location="Sao Paulo"),
        )

        result = match(provider, "como está o clima hoje")

        assert result.intent_name == "local_weather"
        assert "20 graus" in result.reply_text


# ── local_vision_* ───────────────────────────────────────────────────────────

class TestLocalVision:
    def test_what_are_you_seeing_uses_vision_observation(self):
        vision = FakeVisionClient()
        provider = LocalIntentProvider(vision_client=vision)

        result = match(provider, "o que você está vendo")

        assert result.intent_name == "local_vision_scene"
        assert "640 por 480" in result.reply_text
        assert "contraste 255" in result.reply_text
        assert vision.calls == 1

    def test_light_question_formats_luma(self):
        vision = FakeVisionClient(
            VisionObservation(
                valid=True,
                scene="dim",
                timestamp_ms=1,
                width=640,
                height=480,
                jpeg_bytes=1000,
                capture_ms=100,
                luma_avg=60,
                luma_min=0,
                luma_max=140,
                contrast=140,
                motion_score=0,
            )
        )
        provider = LocalIntentProvider(vision_client=vision)

        result = match(provider, "como está a luz")

        assert result.intent_name == "local_vision_light"
        assert "pouca luz" in result.reply_text
        assert "60 de 255" in result.reply_text

    def test_motion_question_formats_motion_score(self):
        vision = FakeVisionClient(
            VisionObservation(
                valid=True,
                scene="normal",
                timestamp_ms=1,
                width=640,
                height=480,
                jpeg_bytes=1000,
                capture_ms=100,
                luma_avg=120,
                luma_min=0,
                luma_max=255,
                contrast=255,
                motion_score=28,
            )
        )
        provider = LocalIntentProvider(vision_client=vision)

        result = match(provider, "tem movimento aí")

        assert result.intent_name == "local_vision_motion"
        assert "movimento forte" in result.reply_text
        assert "28" in result.reply_text

    def test_person_question_reports_detector_unavailable(self):
        provider = LocalIntentProvider(vision_client=FakeVisionClient())

        result = match(provider, "você está me vendo")

        assert result.intent_name == "local_vision_person"
        assert "detector de rosto" in result.reply_text

    def test_person_question_reports_detected_face(self):
        obs = FakeVisionClient().observation
        analysis = VisionAnalysis(
            observation=obs,
            detector="test",
            detector_available=True,
            face_detected=True,
            face_count=1,
            primary_face=FaceBox(x=260, y=120, width=100, height=120),
        )
        provider = LocalIntentProvider(vision_client=FakeVisionClient(analysis=analysis))

        result = match(provider, "você está me vendo")

        assert result.intent_name == "local_vision_person"
        assert "Detectei um rosto" in result.reply_text
        assert "centro" in result.reply_text

    def test_vision_unavailable_still_matches_intent(self):
        provider = LocalIntentProvider(vision_client=FakeVisionClient(error=VisionError("offline")))

        result = match(provider, "o que você vê")

        assert result.intent_name == "local_vision_scene"
        assert "camera" in result.reply_text.lower()


# ── local_mood ────────────────────────────────────────────────────────────────

class TestLocalMood:
    _phrases = [
        "como você está",
        "como voce esta",
        "tudo bem",
        "tudo certo",
        "como você se sente",
        "como voce se sente",
        "está feliz",
        "seu humor",
        "está bem",
    ]

    @pytest.mark.parametrize("text", _phrases)
    def test_phrases_match(self, provider, text):
        result = match(provider, text)
        assert result.intent_name == "local_mood"

    def test_expression_happy(self, provider):
        result = match(provider, "está feliz")
        assert result.expression_id == 3  # HAPPY

    def test_social_wellbeing_is_not_technical_status(self, provider):
        ctx = {"status": {"health": 85, "attention": 0.72}}
        result = match(provider, "como voce esta", context=ctx)

        assert result.intent_name == "local_mood"
        assert "operacional" not in result.reply_text.lower()
        assert "Status:" not in result.reply_text

    def test_social_wellbeing_reflects_low_valence(self, provider):
        ctx = {"status": {"valence": -0.6, "activation": 0.4, "attention": 0.6}}
        result = match(provider, "como voce esta", context=ctx)

        assert result.intent_name == "local_mood"
        assert "atravessado" in result.reply_text
        assert result.expression_id == 7

    def test_social_wellbeing_reflects_high_activation(self, provider):
        ctx = {"status": {"valence": 0.1, "activation": 0.8, "attention": 0.6}}
        result = match(provider, "tudo bem", context=ctx)

        assert result.intent_name == "local_mood"
        assert "acordado" in result.reply_text
        assert result.expression_id == 1


# ── local_expression_* ───────────────────────────────────────────────────────

class TestLocalExpression:
    def test_short_happy_keyword(self, provider):
        result = match(provider, "feliz")
        assert result.intent_name == "local_expression_happy"
        assert result.expression_id == 3

    def test_happy_synonym(self, provider):
        result = match(provider, "sorria")
        assert result.intent_name == "local_expression_happy"
        assert result.expression_id == 3

    def test_curious_keyword(self, provider):
        result = match(provider, "modo curioso")
        assert result.intent_name == "local_expression_curious"
        assert result.expression_id == 4


# ── local_greeting ────────────────────────────────────────────────────────────

class TestLocalGreeting:
    _phrases = [
        "olá",
        "oi noisebot",   # "oi " com espaço após normalização
        "bom dia",
        "boa tarde",
        "boa noite",
        "hey robô",
        "hello",
        "hi there",
    ]

    @pytest.mark.parametrize("text", _phrases)
    def test_phrases_match(self, provider, text):
        result = match(provider, text)
        assert result.intent_name == "local_greeting"

    def test_expression_happy(self, provider):
        result = match(provider, "olá")
        assert result.expression_id == 3  # HAPPY

    def test_reply_not_empty(self, provider):
        result = match(provider, "oi noisebot")
        assert result.reply_text and len(result.reply_text) > 0


# ── local_farewell ────────────────────────────────────────────────────────────

class TestLocalFarewell:
    _phrases = [
        "tchau",
        "até logo",
        "até mais",
        "bye",
        "adeus",
        "pode parar",
        "para de ouvir",
    ]

    @pytest.mark.parametrize("text", _phrases)
    def test_phrases_match(self, provider, text):
        result = match(provider, text)
        assert result.intent_name == "local_farewell"


# ── local_look_* ──────────────────────────────────────────────────────────────

class TestLocalLook:
    def test_look_up(self, provider):
        result = match(provider, "olha pra cima")
        assert result.intent_name == "local_look_up"
        assert result.action_id == 3  # look_up

    def test_look_up_alt(self, provider):
        result = match(provider, "olha para cima")
        assert result.intent_name == "local_look_up"

    def test_look_down(self, provider):
        result = match(provider, "olha pra baixo")
        assert result.intent_name == "local_look_down"
        assert result.action_id == 4  # look_down

    def test_look_down_alt(self, provider):
        result = match(provider, "olha abaixo")
        assert result.intent_name == "local_look_down"

    def test_look_left(self, provider):
        result = match(provider, "olha pra esquerda")
        assert result.intent_name == "local_look_left"

    def test_look_right(self, provider):
        result = match(provider, "olha pra direita")
        assert result.intent_name == "local_look_right"

    def test_look_intents_have_no_reply_text(self, provider):
        """Intents de olhar não têm reply_text (só movimento)."""
        for text in ["olha pra cima", "olha pra baixo", "olha pra esquerda", "olha pra direita"]:
            result = match(provider, text)
            assert result.reply_text is None, f"{text}: reply_text deveria ser None"


# ── local_volume_up / down ────────────────────────────────────────────────────

class TestLocalVolume:
    def test_volume_up(self, provider):
        result = match(provider, "aumenta o volume")
        assert result.intent_name == "local_volume_up"

    def test_volume_up_alt(self, provider):
        result = match(provider, "fala mais alto")
        assert result.intent_name == "local_volume_up"

    def test_volume_down(self, provider):
        result = match(provider, "diminui o volume")
        assert result.intent_name == "local_volume_down"

    def test_volume_down_alt(self, provider):
        result = match(provider, "fala mais baixo")
        assert result.intent_name == "local_volume_down"

    def test_silencio(self, provider):
        result = match(provider, "silêncio")
        assert result.intent_name == "local_volume_down"


# ── Agenda local → firmware ---------------------------------------------------

class TestLocalAgendaCommands:
    def test_alert_silence(self, provider):
        result = match(provider, "silencia o alarme")
        assert result.intent_name == "local_alert_silence"
        assert result.device_command["event"] == "ALERT_COMMAND"
        assert result.device_command["action"] == "silence"

    def test_timer_create_simple(self, provider):
        result = match(provider, "timer de 15 minutos")
        assert result.intent_name == "local_timer_create"
        assert result.device_command["event"] == "AGENDA_COMMAND"
        assert result.device_command["action"] == "timer_create"
        assert result.device_command["duration_ms"] == 15 * 60 * 1000

    def test_timer_create_named(self, provider):
        result = match(provider, "cria um timer chamado chá de 4 minutos")
        assert result.intent_name == "local_timer_create"
        assert result.device_command["label"] == "cha"
        assert result.device_command["duration_ms"] == 4 * 60 * 1000

    def test_timer_cancel_named(self, provider):
        result = match(provider, "cancela o timer do chá")
        assert result.intent_name == "local_timer_cancel"
        assert result.device_command["action"] == "timer_cancel"
        assert result.device_command["label"] == "cha"

    @pytest.mark.parametrize("text", [
        "cancele o timer",
        "para o temporizador",
        "desliga o timing",
        "encerra a contagem",
    ])
    def test_timer_cancel_without_label_variants(self, provider, text):
        result = match(provider, text)
        assert result.intent_name == "local_timer_cancel"
        assert result.device_command["action"] == "timer_cancel"
        assert result.device_command["label"] == ""

    @pytest.mark.parametrize("text", [
        "pare o timer do cha",
        "cancele o temporizador do cha",
        "encerra o timing chamado cha",
    ])
    def test_timer_cancel_named_variants(self, provider, text):
        result = match(provider, text)
        assert result.intent_name == "local_timer_cancel"
        assert result.device_command["action"] == "timer_cancel"
        assert result.device_command["label"] == "cha"

    def test_reminder_create(self, provider):
        result = match(provider, "me lembre de tomar água daqui a 15 minutos")
        assert result.intent_name == "local_reminder_create"
        assert result.device_command["action"] == "reminder_create"
        assert result.device_command["label"] == "tomar agua"
        assert result.device_command["delay_ms"] == 15 * 60 * 1000

    def test_alarm_create_weekdays(self, provider):
        result = match(provider, "cria um alarme às 7 e meia de segunda a sexta")
        assert result.intent_name == "local_alarm_create"
        assert result.device_command["action"] == "alarm_create"
        assert result.device_command["hour"] == 7
        assert result.device_command["minute"] == 30
        assert result.device_command["weekdays_mask"] == 0x3E

    def test_alarm_disable_morning(self, provider):
        result = match(provider, "desativa o alarme da manhã")
        assert result.intent_name == "local_alarm_disable"
        assert result.device_command["action"] == "alarm_set_enabled"
        assert result.device_command["label"] == "manha"
        assert result.device_command["enabled"] is False


# ── Normalização ──────────────────────────────────────────────────────────────

class TestNormalization:
    def test_accents_stripped(self, provider):
        """'olá' e 'ola' casam o mesmo intent."""
        r1 = match(provider, "olá")
        r2 = match(provider, "ola")
        assert r1.intent_name == r2.intent_name == "local_greeting"

    def test_uppercase(self, provider):
        result = match(provider, "QUE HORAS SÃO", now=datetime(2024, 1, 1, 10, 0))
        assert result.intent_name == "local_time"

    def test_punctuation_stripped(self, provider):
        result = match(provider, "ola!!!")
        assert result.intent_name == "local_greeting"

    def test_mixed_accents_and_case(self, provider):
        result = match(provider, "Até Mais!")
        assert result.intent_name == "local_farewell"


# ── IntentResolved estrutura ──────────────────────────────────────────────────

class TestIntentResolvedStructure:
    def test_has_intent_true_when_intent_name_set(self, provider):
        result = match(provider, "tudo bem")
        assert result.has_intent is True

    def test_has_intent_false_when_no_match(self, provider):
        result = match(provider, "xyz nada encontrado aqui")
        assert result.has_intent is False

    def test_result_is_frozen(self, provider):
        result = match(provider, "tudo bem")
        with pytest.raises((AttributeError, TypeError)):
            result.intent_name = "outro"  # type: ignore[misc]
