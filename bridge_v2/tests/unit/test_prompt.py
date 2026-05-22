"""Tests for bridgev2.llm.prompt."""
from __future__ import annotations

import json
import pytest

from bridgev2.llm.prompt import (
    build_messages,
    parse_llm_json,
    recover_llm_reply_text,
    _SYSTEM_PROMPT,
)


# ── build_messages ────────────────────────────────────────────────────────────

class TestBuildMessages:
    def test_returns_two_messages(self):
        msgs = build_messages("oi")
        assert len(msgs) == 2

    def test_system_first(self):
        msgs = build_messages("oi")
        assert msgs[0]["role"] == "system"

    def test_user_second(self):
        msgs = build_messages("oi")
        assert msgs[1]["role"] == "user"
        assert msgs[1]["content"] == "oi"

    def test_default_system_prompt_contains_noisebot(self):
        msgs = build_messages("oi")
        assert "NoiseBot" in msgs[0]["content"]

    def test_default_system_prompt_contains_json_schema(self):
        msgs = build_messages("oi")
        assert "expression_id" in msgs[0]["content"]
        assert "emot_event" in msgs[0]["content"]

    def test_custom_system_prompt(self):
        msgs = build_messages("oi", config={"system_prompt": "Custom prompt"})
        assert msgs[0]["content"] == "Custom prompt"

    def test_no_context_no_extras(self):
        msgs = build_messages("oi")
        # Apenas system + user, sem linhas de contexto extra
        assert "Estado do robô" not in msgs[0]["content"]

    def test_robot_state_context(self):
        msgs = build_messages("oi", context={"robot_state": "IDLE"})
        assert "Estado do robô: IDLE" in msgs[0]["content"]

    def test_emotion_state_context(self):
        msgs = build_messages("oi", context={"emotion_state": "calm"})
        assert "Estado emocional: calm" in msgs[0]["content"]

    def test_both_contexts(self):
        msgs = build_messages(
            "oi",
            context={"robot_state": "SPEAKING", "emotion_state": "happy"},
        )
        content = msgs[0]["content"]
        assert "Estado do robô: SPEAKING" in content
        assert "Estado emocional: happy" in content

    def test_none_context_ok(self):
        msgs = build_messages("hello", context=None)
        assert len(msgs) == 2

    def test_none_config_ok(self):
        msgs = build_messages("hello", config=None)
        assert len(msgs) == 2

    def test_empty_text(self):
        msgs = build_messages("")
        assert msgs[1]["content"] == ""

    def test_multiline_text(self):
        text = "linha 1\nlinha 2"
        msgs = build_messages(text)
        assert msgs[1]["content"] == text


# ── parse_llm_json ────────────────────────────────────────────────────────────

_VALID_JSON = '{"reply":"Olá!","expression_id":1,"action":0,"emot_event":2}'


class TestParseLlmJson:
    def test_valid_json(self):
        r = parse_llm_json(_VALID_JSON)
        assert r["reply"] == "Olá!"
        assert r["expression_id"] == 1
        assert r["action"] == 0
        assert r["emot_event"] == 2

    def test_returns_all_keys(self):
        r = parse_llm_json(_VALID_JSON)
        assert set(r.keys()) == {"reply", "expression_id", "action", "emot_event"}

    def test_markdown_code_block(self):
        raw = "```json\n" + _VALID_JSON + "\n```"
        r = parse_llm_json(raw)
        assert r["reply"] == "Olá!"

    def test_markdown_no_lang(self):
        raw = "```\n" + _VALID_JSON + "\n```"
        r = parse_llm_json(raw)
        assert r["reply"] == "Olá!"

    def test_json_embedded_in_text(self):
        raw = "Aqui está a resposta:\n" + _VALID_JSON + "\nFim."
        r = parse_llm_json(raw)
        assert r["reply"] == "Olá!"

    def test_missing_expression_id_returns_none(self):
        raw = '{"reply":"ok","action":1,"emot_event":2}'
        r = parse_llm_json(raw)
        assert r["expression_id"] is None

    def test_missing_action_returns_none(self):
        raw = '{"reply":"ok","expression_id":0,"emot_event":2}'
        r = parse_llm_json(raw)
        assert r["action"] is None

    def test_missing_emot_event_returns_none(self):
        raw = '{"reply":"ok","expression_id":0,"action":1}'
        r = parse_llm_json(raw)
        assert r["emot_event"] is None

    def test_emot_event_id_alias(self):
        raw = '{"reply":"ok","expression_id":0,"action":1,"emot_event_id":3}'
        r = parse_llm_json(raw)
        assert r["emot_event"] == 3

    def test_string_integers_coerced(self):
        raw = '{"reply":"ok","expression_id":"2","action":"1","emot_event":"2"}'
        r = parse_llm_json(raw)
        assert r["expression_id"] == 2
        assert r["action"] == 1
        assert r["emot_event"] == 2

    def test_null_values_become_none(self):
        raw = '{"reply":"ok","expression_id":null,"action":null,"emot_event":null}'
        r = parse_llm_json(raw)
        assert r["expression_id"] is None
        assert r["action"] is None
        assert r["emot_event"] is None

    def test_missing_reply_returns_empty_string(self):
        raw = '{"expression_id":0,"action":0,"emot_event":2}'
        r = parse_llm_json(raw)
        assert r["reply"] == ""

    def test_invalid_json_raises_value_error(self):
        with pytest.raises((ValueError, Exception)):
            parse_llm_json("isto não é json")

    def test_whitespace_stripped(self):
        raw = "  \n" + _VALID_JSON + "\n  "
        r = parse_llm_json(raw)
        assert r["reply"] == "Olá!"


class TestRecoverLlmReplyText:
    def test_recovers_reply_from_truncated_json(self):
        raw = '{"reply":"Era uma vez um robo pequeno'
        assert recover_llm_reply_text(raw) == "Era uma vez um robo pequeno"

    def test_recovers_escaped_reply_fragment(self):
        raw = r'{"reply":"Ele disse: \"oi\"'
        assert recover_llm_reply_text(raw) == 'Ele disse: "oi"'

    def test_never_returns_raw_json_object(self):
        assert recover_llm_reply_text('{"foo":') == "Não consegui completar minha resposta."

    def test_plain_text_is_returned(self):
        assert recover_llm_reply_text("olá mundo") == "olá mundo"
