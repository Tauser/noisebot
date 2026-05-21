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


@pytest.fixture()
def provider() -> LocalIntentProvider:
    return LocalIntentProvider()


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
        "como você está",
        "como voce esta",
        "tudo bem",
        "tudo certo",
        "seu status",
        "status do sistema",
    ]

    @pytest.mark.parametrize("text", _phrases)
    def test_phrases_match(self, provider, text):
        result = match(provider, text)
        assert result.intent_name == "local_status"

    def test_reply_with_health_and_attention(self, provider):
        ctx = {"status": {"health": 85, "attention": 0.72}}
        result = match(provider, "tudo bem", context=ctx)
        assert "85" in result.reply_text
        assert "72" in result.reply_text

    def test_reply_without_status_context(self, provider):
        result = match(provider, "tudo bem", context={})
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


# ── local_mood ────────────────────────────────────────────────────────────────

class TestLocalMood:
    _phrases = [
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
