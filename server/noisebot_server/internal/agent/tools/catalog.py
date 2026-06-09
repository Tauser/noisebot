"""Tool catalog for server-side LLM tool calls.

Each ToolSpec defines the contract for a tool:
  name           — unique identifier, same string the LLM emits in tool_call.name
  description    — shown to the LLM in the system prompt
  arguments_schema — JSON Schema subset used for validation before execution
  risk_level     — "low" | "confirmation_required" | "blocked"
  allowed_states — FSM states where the tool may run (None = any state)

Tools NOT in this catalog are silently blocked by the gateway.

Blocked by policy (never added to catalog until safety gate is green):
  - Any servo / physical movement (motion_safety not released)
  - delete_memory, factory_reset, change_wifi
  - Actions that write directly to NVS or sensitive persistence
  - Any restart/reboot without explicit user confirmation
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

_EXPRESSION_IDS: list[str] = [
    "neutral", "happy", "curious", "sleepy", "focused",
    "suspicious", "surprised", "sad", "alarmed", "angry",
]


@dataclass(frozen=True)
class ToolSpec:
    name: str
    description: str
    arguments_schema: dict[str, Any]
    risk_level: str  # "low" | "confirmation_required" | "blocked"
    allowed_states: frozenset[str] | None = field(default=None)


CATALOG: dict[str, ToolSpec] = {
    "set_expression": ToolSpec(
        name="set_expression",
        description=(
            "Define a expressão facial do robô imediatamente. "
            "Use para reforçar visualmente o estado emocional da resposta."
        ),
        arguments_schema={
            "type": "object",
            "properties": {
                "expression_id": {
                    "type": "string",
                    "enum": _EXPRESSION_IDS,
                    "description": "Nome semântico da expressão.",
                },
            },
            "required": ["expression_id"],
        },
        risk_level="low",
    ),
    "set_led": ToolSpec(
        name="set_led",
        description=(
            "Define a cor e brilho dos LEDs WS2812 do robô. "
            "Use para feedback visual de estado ou emoção."
        ),
        arguments_schema={
            "type": "object",
            "properties": {
                "color": {
                    "type": "string",
                    "description": (
                        "Cor em hex (#RRGGBB) ou nome: "
                        "'red', 'green', 'blue', 'white', 'yellow', 'off'."
                    ),
                },
                "brightness": {
                    "type": "integer",
                    "minimum": 0,
                    "maximum": 100,
                    "description": "Brilho percentual (0 = apagado, 100 = máximo).",
                },
            },
            "required": ["color"],
        },
        risk_level="low",
    ),
    "create_timer": ToolSpec(
        name="create_timer",
        description=(
            "Cria um temporizador. O robô avisa quando o tempo acabar. "
            "Prefira esta tool quando o usuário pedir 'timer de X minutos/segundos'."
        ),
        arguments_schema={
            "type": "object",
            "properties": {
                "duration_s": {
                    "type": "integer",
                    "minimum": 1,
                    "maximum": 86400,
                    "description": "Duração em segundos (máximo 24 h = 86400 s).",
                },
                "label": {
                    "type": "string",
                    "description": "Rótulo descritivo opcional (ex: 'macarrão').",
                },
            },
            "required": ["duration_s"],
        },
        risk_level="low",
    ),
    "create_reminder": ToolSpec(
        name="create_reminder",
        description=(
            "Cria um lembrete que o robô fala no horário definido. "
            "Prefira esta tool quando o usuário pedir 'me lembra às HH:MM' ou 'lembrete para amanhã'."
        ),
        arguments_schema={
            "type": "object",
            "properties": {
                "text": {
                    "type": "string",
                    "description": "Texto do lembrete (falado pelo robô no horário).",
                },
                "trigger_iso": {
                    "type": "string",
                    "description": (
                        "Horário de disparo em ISO 8601 "
                        "(ex: '2026-06-09T15:30:00'). Use fuso local implícito."
                    ),
                },
            },
            "required": ["text", "trigger_iso"],
        },
        risk_level="low",
    ),
    "analyze_vision": ToolSpec(
        name="analyze_vision",
        description=(
            "Captura e analisa o que a câmera está vendo agora. "
            "Use apenas quando o usuário perguntar explicitamente o que o robô está vendo "
            "ou quando a cena for diretamente relevante para a resposta."
        ),
        arguments_schema={
            "type": "object",
            "properties": {},
            "required": [],
        },
        risk_level="low",
    ),
    "request_confirmation": ToolSpec(
        name="request_confirmation",
        description=(
            "Solicita confirmação explícita do usuário antes de executar uma ação sensível. "
            "Use quando a ação puder ter consequências não triviais e reversíveis."
        ),
        arguments_schema={
            "type": "object",
            "properties": {
                "question": {
                    "type": "string",
                    "description": "Pergunta de confirmação a ser feita ao usuário.",
                },
                "action_description": {
                    "type": "string",
                    "description": "Descrição curta da ação a executar se confirmada.",
                },
            },
            "required": ["question"],
        },
        risk_level="confirmation_required",
    ),
}


def validate_arguments(spec: ToolSpec, arguments: dict[str, Any]) -> list[str]:
    """Validate tool arguments against the spec's JSON Schema subset.

    Returns a list of human-readable error strings. Empty = valid.
    Covers: required fields, type checking, enum values, numeric ranges.
    Does NOT require the jsonschema library.
    """
    errors: list[str] = []
    schema = spec.arguments_schema
    props: dict[str, Any] = schema.get("properties", {})
    required: list[str] = schema.get("required", [])

    for req in required:
        if req not in arguments:
            errors.append(f"argumento obrigatorio ausente: '{req}'")

    for arg_name, value in arguments.items():
        if arg_name not in props:
            errors.append(f"argumento desconhecido: '{arg_name}'")
            continue
        errors.extend(_validate_value(arg_name, value, props[arg_name]))

    return errors


def _validate_value(name: str, value: Any, prop: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    expected_type = prop.get("type")

    if expected_type == "string":
        if not isinstance(value, str):
            errors.append(
                f"'{name}' deve ser string, recebido {type(value).__name__}"
            )
        elif "enum" in prop and value not in prop["enum"]:
            errors.append(
                f"'{name}' valor '{value}' invalido; permitidos: {prop['enum']}"
            )
    elif expected_type == "integer":
        if not isinstance(value, int) or isinstance(value, bool):
            errors.append(
                f"'{name}' deve ser inteiro, recebido {type(value).__name__}"
            )
        else:
            minimum = prop.get("minimum")
            maximum = prop.get("maximum")
            if minimum is not None and value < minimum:
                errors.append(f"'{name}' ({value}) abaixo do minimo ({minimum})")
            if maximum is not None and value > maximum:
                errors.append(f"'{name}' ({value}) acima do maximo ({maximum})")
    elif expected_type == "boolean":
        if not isinstance(value, bool):
            errors.append(
                f"'{name}' deve ser booleano, recebido {type(value).__name__}"
            )
    elif expected_type == "number":
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            errors.append(
                f"'{name}' deve ser numero, recebido {type(value).__name__}"
            )

    return errors


__all__ = ["CATALOG", "ToolSpec", "validate_arguments"]
