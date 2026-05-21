"""bridgev2.llm.prompt — Montagem de mensagens para LLM providers.

build_messages:  constrói lista de mensagens no formato OpenAI/Gemini chat.
parse_llm_json:  extrai e valida JSON de resposta do LLM.
"""
from __future__ import annotations

import json
import re
from typing import Any

# ── System prompt ─────────────────────────────────────────────────────────────

_SYSTEM_PROMPT = (
    "Você é NoiseBot, um companion robot expressivo de mesa.\n"
    "Personalidade: caloroso, curioso, expressivo, respostas < 10s de fala.\n"
    "\n"
    "Responda SEMPRE em JSON válido, sem markdown, neste formato exato:\n"
    '{"reply":"<texto falado>","expression_id":<int>,"action":<int>,"emot_event":<int>}\n'
    "\n"
    "expression_id: 0=neutro 1=feliz 2=curioso 3=sonolento 4=focado "
    "5=desconfiado 6=surpreso 7=triste 8=alarmado 9=bravo\n"
    "action: 0=greet 1=nod 2=shake 3=look_up 4=look_down\n"
    "emot_event: 2=voice_start 3=audio_started\n"
    "\n"
    '"reply" deve ser natural, conciso, máximo 2-3 frases curtas.'
)


def build_messages(
    text: str,
    context: dict[str, Any] | None = None,
    config: dict[str, Any] | None = None,
) -> list[dict[str, str]]:
    """Constrói lista de mensagens no formato OpenAI/Gemini chat.

    Parâmetros
    ----------
    text:
        Transcrição de fala do usuário (já processada pelo STT).
    context:
        Contexto opcional: turn_id, robot_state, emotion_state, etc.
    config:
        Configuração opcional: pode sobrescrever 'system_prompt'.

    Retorna
    -------
    list[dict]:
        [{"role": "system", "content": ...}, {"role": "user", "content": ...}]
    """
    cfg = config or {}
    ctx = context or {}

    system_content: str = cfg.get("system_prompt", _SYSTEM_PROMPT)

    # Append optional context hints
    extra: list[str] = []
    if ctx.get("robot_state"):
        extra.append(f"Estado do robô: {ctx['robot_state']}")
    if ctx.get("emotion_state"):
        extra.append(f"Estado emocional: {ctx['emotion_state']}")

    if extra:
        system_content = system_content.rstrip() + "\n\n" + "\n".join(extra)

    return [
        {"role": "system", "content": system_content},
        {"role": "user", "content": text},
    ]


def parse_llm_json(raw: str) -> dict[str, Any]:
    """Extrai e valida o JSON de resposta do LLM.

    Aceita raw com ou sem bloco markdown (```json...```).
    Retorna dict com chaves: reply (str), expression_id (int|None),
    action (int|None), emot_event (int|None).

    Lança ValueError se não encontrar JSON válido.
    """
    cleaned = raw.strip()

    # Strip markdown code block if present
    md = re.search(r"```(?:json)?\s*([\s\S]*?)```", cleaned)
    if md:
        cleaned = md.group(1).strip()

    # Direct parse
    try:
        data = json.loads(cleaned)
    except json.JSONDecodeError:
        # Try to extract first {...} block
        obj = re.search(r"\{[\s\S]*\}", cleaned)
        if not obj:
            raise ValueError(f"Sem JSON válido em: {raw!r}")
        data = json.loads(obj.group(0))

    return {
        "reply": str(data.get("reply", "")),
        "expression_id": _int_or_none(data.get("expression_id")),
        "action": _int_or_none(data.get("action")),
        "emot_event": _int_or_none(
            data.get("emot_event") or data.get("emot_event_id")
        ),
    }


def _int_or_none(val: Any) -> int | None:
    if val is None:
        return None
    try:
        return int(val)
    except (ValueError, TypeError):
        return None
