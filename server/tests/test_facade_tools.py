from __future__ import annotations
import asyncio
import importlib
import io
import json
import logging
import math
import struct
from pathlib import Path
from urllib.error import HTTPError
import pytest

from _facade_common import _drain_queue, _make_server_config, _server_loud_pcm, _simulate_server_voice_session, _wait_until


def test_llm_prompt_includes_recent_replies_to_avoid_repetition() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    messages = llm.build_messages(
        "Me conte uma piada.",
        {"recent_replies": ["Por que o livro foi ao médico? Porque tinha muitos problemas de capa."]},
    )

    system = messages[0]["content"]
    assert "Respostas recentes a evitar repetir" in system
    assert "livro foi ao médico" in system
    assert "nunca repita" in system

def test_llm_prompt_includes_current_user_profile() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    messages = llm.build_messages(
        "Como estou hoje?",
        {
            "user_profile": {
                "display_name": "Tadeu",
                "relationship": "owner",
                "language": "pt-BR",
                "robot_nickname": "Noise",
                "persona_mode": "companion",
                "interaction_style": "direct_warm",
            }
        },
    )

    system = messages[0]["content"]
    assert "Perfil do usuario atual" in system
    assert "Nome do usuario: Tadeu" in system
    assert "Nome/apelido do robo para este usuario: Noise" in system
    assert "Como se comportar com este usuario:" in system
    assert "acolhedor e presente" in system
    assert "calor humano no tom" in system
    assert "nao invente outra identidade" in system

def test_personality_prompt_lines_compose_known_axes() -> None:
    personality = importlib.import_module("noisebot_server.internal.agent.personality")

    lines = personality.personality_prompt_lines("playful", "curious")

    assert lines[0] == "Como se comportar com este usuario:"
    assert any("humor leve" in line for line in lines)
    assert any("interesse genuino" in line for line in lines)

def test_personality_prompt_lines_skip_unknown_values() -> None:
    personality = importlib.import_module("noisebot_server.internal.agent.personality")

    assert personality.personality_prompt_lines("", "") == []
    assert personality.personality_prompt_lines("modo_inexistente", "estilo_inexistente") == []

    partial = personality.personality_prompt_lines("companion", "estilo_inexistente")
    assert partial == [
        "Como se comportar com este usuario:",
        "- Seja acolhedor e presente; acompanhe o assunto do usuario sem se impor "
        "ou desviar para outros temas.",
    ]

def test_llm_language_guard_replaces_foreign_script_reply() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    reply, replaced = llm.enforce_pt_br_reply(
        "绿是程序员的最爱，因为蓝（绿）！",
        "Me conte uma piada.",
    )

    assert replaced
    assert "Por que" in reply
    assert "绿" not in reply

def test_llm_language_guard_replaces_english_reply() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    reply, replaced = llm.enforce_pt_br_reply(
        "Did you know that penguins can't fly? They're amazing swimmers instead!",
        "Me diga uma curiosidade curta.",
    )

    assert replaced
    assert "Curiosidade:" in reply
    assert "penguins" not in reply

def test_llm_language_guard_replaces_english_curiosity_with_fact() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    reply, replaced = llm.enforce_pt_br_reply(
        "Did you know that penguins can't fly? They're amazing swimmers instead!",
        "Me conte uma curiosidade.",
    )

    assert replaced
    assert "Curiosidade:" in reply
    assert "idioma errado" not in reply
    assert "penguins" not in reply

def test_server_app_state_persists_device_persona_profile(tmp_path) -> None:
    app_state = importlib.import_module("noisebot_server.internal.ops.app_state")
    state_path = tmp_path / "app_state.json"

    store = app_state.AppStateStore(state_path)
    persona = store.update_device_persona({
        "warmth": 1.4,
        "user": {
            "id": "tadeu",
            "display_name": "Tadeu",
            "relationship": "criador",
            "language": "pt-BR",
            "robot_nickname": "Noise",
            "persona_mode": "companheiro",
            "interaction_style": "direto_afetuoso",
        },
    })

    assert persona["warmth"] == 1.0
    assert persona["user"]["display_name"] == "Tadeu"
    assert persona["user"]["robot_nickname"] == "Noise"

    reloaded = app_state.AppStateStore(state_path)
    snapshot = reloaded.snapshot()

    assert snapshot["device_persona"]["user"]["id"] == "tadeu"
    assert snapshot["device_persona"]["user"]["display_name"] == "Tadeu"
    assert snapshot["device_persona"]["user"]["interaction_style"] == "direto_afetuoso"

async def test_server_device_persona_endpoint_uses_cache_when_firmware_offline(
    tmp_path,
    monkeypatch,
) -> None:
    http = importlib.import_module("noisebot_server.internal.ops.http")
    app_state = importlib.import_module("noisebot_server.internal.ops.app_state")

    class FakeRequest:
        async def json(self):
            return {
                "user": {
                    "id": "tadeu",
                    "display_name": "Tadeu",
                    "robot_nickname": "Noise",
                }
            }

    store = app_state.AppStateStore(tmp_path / "app_state.json")
    server = http.OpsHttpServer.__new__(http.OpsHttpServer)
    server._app_state = store
    server._firmware_diag_client = None
    server._token = "secret"
    monkeypatch.setattr(http, "check_token", lambda request, expected: True)

    put_response = await server._put_device_persona(FakeRequest())
    put_payload = json.loads(put_response.text)
    get_response = await server._get_device_persona(None)
    get_payload = json.loads(get_response.text)

    assert put_response.status == 200
    assert put_payload["source"] == "server_cache"
    assert put_payload["firmware_applied"] is False
    assert get_payload["source"] == "server_cache"
    assert get_payload["user"]["id"] == "tadeu"
    assert get_payload["user"]["display_name"] == "Tadeu"
    assert get_payload["user"]["robot_nickname"] == "Noise"

def test_server_agent_local_intent_matches_time() -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match("que horas sao", turn_id=44)

    assert result.intent_name == "local_time"
    assert result.reply_text

def test_server_agent_local_intent_answers_curiosity_in_pt_br() -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match("me conte uma curiosidade", turn_id=44)

    assert result.intent_name == "local_curiosity_fact"
    assert result.reply_text
    assert "Curiosidade:" in result.reply_text
    assert "idioma errado" not in result.reply_text
    assert result.expression_id == 2

def test_server_agent_local_intent_handles_bare_stop_without_llm() -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match("Pare.", turn_id=51)

    assert result.intent_name == "local_stop"
    assert result.reply_text == "Pronto, parei."
    assert result.expression_id == 0
    assert result.resolution_reason == "direct_stop"

@pytest.mark.parametrize(
    "phrase",
    [
        "Corta.",
        "Corta isso.",
        "Para de falar.",
        "Chega disso.",
        "Nao quero mais.",
        "Encerra.",
    ],
)
def test_server_agent_local_intent_handles_expanded_stop_phrases(phrase: str) -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match(phrase, turn_id=46)

    assert result.intent_name == "local_stop"
    assert result.reply_text == "Pronto, parei."
    assert result.resolution_reason == "direct_stop"

def test_server_agent_light_color_intent_emits_led_command() -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match("deixe os leds azuis", turn_id=53)

    assert result.intent_name == "local_light_color"
    assert result.reply_text == "Luzes em azul."
    assert result.device_command == {
        "event": "LED_COMMAND",
        "action": "color",
        "r": 40,
        "g": 120,
        "b": 255,
    }

def test_server_orchestrator_loads_user_profile_for_llm_context() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )

    class PersonaClient:
        def persona(self):
            return {
                "user": {
                    "id": "owner",
                    "display_name": "Tadeu",
                    "relationship": "owner",
                    "language": "pt-BR",
                    "robot_nickname": "Noise",
                    "persona_mode": "companion",
                    "interaction_style": "direct_warm",
                }
            }

    orchestrator = orchestrator_module.Orchestrator(runtime.EventBus())
    orchestrator._persona_sync._firmware_persona = PersonaClient()

    profile = asyncio.run(orchestrator._current_user_profile())

    assert profile["display_name"] == "Tadeu"
    assert profile["robot_nickname"] == "Noise"
    assert profile["interaction_style"] == "direct_warm"

def test_parse_llm_json_valid_expression_string() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    parsed = llm.parse_llm_json('{"expression_id":"happy","reply":"Oi!","tool_call":null}')

    assert parsed["expression_id"] == "happy"
    assert parsed["reply"] == "Oi!"
    assert parsed["tool_call"] is None

def test_parse_llm_json_all_valid_expressions_accepted() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")
    valid = [
        "neutral", "happy", "curious", "sleepy", "focused",
        "suspicious", "surprised", "sad", "alarmed", "angry",
    ]

    for expr in valid:
        parsed = llm.parse_llm_json(
            json.dumps({"expression_id": expr, "reply": "ok", "tool_call": None})
        )
        assert parsed["expression_id"] == expr, f"expressao '{expr}' deve ser aceita"

def test_parse_llm_json_unknown_expression_returns_none() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    parsed = llm.parse_llm_json('{"expression_id":"energized","reply":"Oi!","tool_call":null}')

    assert parsed["expression_id"] is None

def test_parse_llm_json_int_expression_fallback() -> None:
    """Modelos que ainda emitem int legado devem ser aceitos."""
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    parsed = llm.parse_llm_json('{"expression_id":1,"reply":"Oi!","tool_call":null}')

    assert parsed["expression_id"] == "happy"

def test_parse_llm_json_tool_call_absent_returns_none() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    parsed = llm.parse_llm_json('{"expression_id":"neutral","reply":"Ola"}')

    assert parsed["tool_call"] is None

def test_parse_llm_json_tool_call_null_returns_none() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    parsed = llm.parse_llm_json(
        '{"expression_id":"neutral","reply":"Ola","tool_call":null}'
    )

    assert parsed["tool_call"] is None

def test_parse_llm_json_tool_call_valid_structure() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    raw = json.dumps({
        "expression_id": "focused",
        "reply": "Vou verificar.",
        "tool_call": {"name": "set_expression", "arguments": {"expression_id": "focused"}},
    })
    parsed = llm.parse_llm_json(raw)

    assert parsed["tool_call"] is not None
    assert parsed["tool_call"]["name"] == "set_expression"
    assert parsed["tool_call"]["arguments"] == {"expression_id": "focused"}

def test_parse_llm_json_tool_call_without_name_returns_none() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    raw = json.dumps({
        "expression_id": "neutral",
        "reply": "ok",
        "tool_call": {"arguments": {"x": 1}},
    })
    parsed = llm.parse_llm_json(raw)

    assert parsed["tool_call"] is None

def test_parse_llm_json_raises_on_no_json() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    try:
        llm.parse_llm_json("Claro! Posso te ajudar com isso.")
        assert False, "deveria ter levantado ValueError"
    except ValueError:
        pass

def test_parse_llm_json_extracts_from_markdown_fence() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    raw = '```json\n{"expression_id":"curious","reply":"Interessante!","tool_call":null}\n```'
    parsed = llm.parse_llm_json(raw)

    assert parsed["expression_id"] == "curious"
    assert parsed["reply"] == "Interessante!"

def test_parse_llm_json_extracts_embedded_object() -> None:
    """Texto antes/depois do JSON ainda deve ser extraído."""
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    raw = 'Claro! {"expression_id":"happy","reply":"Oi!","tool_call":null} tchau'
    parsed = llm.parse_llm_json(raw)

    assert parsed["expression_id"] == "happy"

def test_llm_system_prompt_has_unified_envelope() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    assert "expression_id" in llm._SYSTEM_PROMPT
    assert "tool_call" in llm._SYSTEM_PROMPT
    assert "reply" in llm._SYSTEM_PROMPT

def test_llm_system_prompt_lists_expression_strings() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    for expr in ("neutral", "happy", "curious", "alarmed", "angry"):
        assert expr in llm._SYSTEM_PROMPT, f"'{expr}' deve estar no _SYSTEM_PROMPT"

def test_llm_system_prompt_has_few_shot_examples() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    # Deve conter pelo menos dois objetos JSON de exemplo
    examples = llm._SYSTEM_PROMPT.count('"tool_call"')
    assert examples >= 2, "prompt deve ter pelo menos dois exemplos com tool_call"

def test_llm_system_prompt_no_raw_ints_as_expression() -> None:
    """Prompt não deve ensinar o modelo a emitir ints crus como expression_id."""
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    assert "expression_id:<int>" not in llm._SYSTEM_PROMPT
    assert "expression_id:0" not in llm._SYSTEM_PROMPT

def test_payload_tools_empty_omits_tools_key() -> None:
    pb = importlib.import_module("noisebot_server.internal.agent.payload_builder")

    payload = pb.build_turn_payload(text="oi", turn_id=1, allowed_tools=None)

    assert "tools" not in payload

def test_payload_tools_present_when_provided() -> None:
    pb = importlib.import_module("noisebot_server.internal.agent.payload_builder")

    payload = pb.build_turn_payload(
        text="oi", turn_id=1, allowed_tools=["set_expression", "set_led"]
    )

    assert payload["tools"] == ["set_expression", "set_led"]

def test_tool_catalog_all_required_fields() -> None:
    catalog = importlib.import_module("noisebot_server.internal.agent.tools.catalog")

    for name, spec in catalog.CATALOG.items():
        assert spec.name == name, f"{name}: spec.name deve bater com a chave"
        assert spec.description, f"{name}: description nao pode ser vazia"
        assert isinstance(spec.arguments_schema, dict), f"{name}: arguments_schema deve ser dict"
        assert spec.risk_level in ("low", "confirmation_required", "blocked"), (
            f"{name}: risk_level invalido: {spec.risk_level}"
        )

def test_tool_catalog_expected_tools_present() -> None:
    catalog = importlib.import_module("noisebot_server.internal.agent.tools.catalog")
    expected = {
        "set_expression", "set_led", "create_timer",
        "create_reminder", "analyze_vision", "request_confirmation",
    }

    assert expected.issubset(set(catalog.CATALOG.keys()))

def test_tool_catalog_all_have_executors() -> None:
    tools = importlib.import_module("noisebot_server.internal.agent.tools")

    for name in tools.CATALOG:
        assert name in tools.EXECUTOR_MAP, f"'{name}' sem executor em EXECUTOR_MAP"

def test_tool_catalog_blocked_tools_absent() -> None:
    """Tools de risco alto não devem estar no catálogo."""
    catalog = importlib.import_module("noisebot_server.internal.agent.tools.catalog")
    blocked = {
        "move_servo", "factory_reset", "delete_memory",
        "change_wifi", "restart", "reboot",
    }

    for name in blocked:
        assert name not in catalog.CATALOG, f"'{name}' nao deve estar no catalogo"

def test_all_other_tools_are_low_risk() -> None:
    catalog = importlib.import_module("noisebot_server.internal.agent.tools.catalog")
    low_risk = {"set_expression", "set_led", "create_timer", "create_reminder", "analyze_vision"}

    for name in low_risk:
        assert catalog.CATALOG[name].risk_level == "low", f"'{name}' deveria ser low risk"

def test_gateway_unknown_tool_vetoed() -> None:
    gateway = importlib.import_module("noisebot_server.internal.agent.tools.gateway")

    result = gateway.execute_tool_call({"name": "launch_missile", "arguments": {}})

    assert result.vetoed is True
    assert result.success is False
    assert result.error is not None
    assert "launch_missile" in result.error

def test_gateway_tool_result_has_no_raw_floats_in_audit() -> None:
    """audit_log não deve vazar floats de telemetria."""
    gateway = importlib.import_module("noisebot_server.internal.agent.tools.gateway")

    result = gateway.execute_tool_call(
        {"name": "set_expression", "arguments": {"expression_id": "sad"}},
        turn_id=7,
    )

    audit_str = str(result.audit_log)
    # Verificar que não há valores float típicos de VAA
    for forbidden in ("valence", "activation", "attention"):
        assert forbidden not in audit_str

def test_format_tool_result_injection_success() -> None:
    """_format_tool_result_injection descreve sucesso em linguagem natural."""
    llm_module = importlib.import_module("noisebot_server.internal.agent.llm")

    msg = llm_module._format_tool_result_injection({
        "tool_name": "create_timer",
        "success": True,
        "vetoed": False,
        "result": {"timer_id": "abc", "duration_s": 300, "label": "Estudos"},
        "error": None,
        "veto_reason": None,
    })

    assert "create_timer" in msg
    assert "sucesso" in msg
    assert "duration_s=300" in msg or "timer_id=abc" in msg
    assert "tool_call" in msg  # instrução para não usar tool_call

def test_format_tool_result_injection_vetoed() -> None:
    """_format_tool_result_injection descreve veto em linguagem natural."""
    llm_module = importlib.import_module("noisebot_server.internal.agent.llm")

    msg = llm_module._format_tool_result_injection({
        "tool_name": "set_led",
        "success": False,
        "vetoed": True,
        "result": None,
        "error": "led bloqueado",
        "veto_reason": "estado incompativel",
    })

    assert "set_led" in msg
    assert "bloqueada" in msg
    assert "estado incompativel" in msg
    assert "tool_call" in msg

def test_format_tool_result_injection_error() -> None:
    """_format_tool_result_injection descreve falha em linguagem natural."""
    llm_module = importlib.import_module("noisebot_server.internal.agent.llm")

    msg = llm_module._format_tool_result_injection({
        "tool_name": "analyze_vision",
        "success": False,
        "vetoed": False,
        "result": None,
        "error": "timeout na camera",
        "veto_reason": None,
    })

    assert "analyze_vision" in msg
    assert "falhou" in msg
    assert "timeout na camera" in msg

def test_format_tool_result_injection_strips_complex_result_values() -> None:
    """Dicts e listas aninhados no result não aparecem crus na injeção."""
    llm_module = importlib.import_module("noisebot_server.internal.agent.llm")

    msg = llm_module._format_tool_result_injection({
        "tool_name": "analyze_vision",
        "success": True,
        "vetoed": False,
        "result": {
            "scene": "sala",
            "nested": {"raw": [1, 2, 3]},  # deve ser ignorado
            "brightness": "alta",
        },
        "error": None,
        "veto_reason": None,
    })

    assert "scene=sala" in msg or "brightness=alta" in msg
    # nested dict não deve aparecer cru
    assert "[1, 2, 3]" not in msg
