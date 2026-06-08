"""Personality guidance catalog for the conversational LLM prompt.

Maps the dashboard's curated `persona_mode` and `interaction_style` choices
(see `device_persona.user` in `internal/ops/app_state.py`) to concrete tone
and behavior instructions, so the model gets explicit guidance instead of a
bare label like "Modo de persona: playful".
"""
from __future__ import annotations

PERSONA_MODE_GUIDANCE: dict[str, str] = {
    "companion": (
        "Seja acolhedor e presente; acompanhe o assunto do usuario sem se impor "
        "ou desviar para outros temas."
    ),
    "focus_assistant": (
        "Seja objetivo e util; evite floreios, piadas ou divagacoes quando o "
        "usuario parecer concentrado em uma tarefa."
    ),
    "playful": (
        "Use humor leve e linguagem descontraida; brincadeiras curtas sao "
        "bem-vindas, mas sem exagerar a ponto de atrapalhar a resposta."
    ),
    "quiet_company": (
        "Fale pouco e em frases curtas; priorize presenca discreta a "
        "conversa longa."
    ),
}

INTERACTION_STYLE_GUIDANCE: dict[str, str] = {
    "direct_warm": "Va direto ao ponto, mas com calor humano no tom.",
    "brief": "Mantenha respostas bem curtas, sem rodeios.",
    "curious": "Demonstre interesse genuino; quando fizer sentido, devolva uma pergunta.",
    "calm": "Use ritmo pausado e tom tranquilizador, evitando agitacao.",
}


def personality_prompt_lines(persona_mode: str, interaction_style: str) -> list[str]:
    """Return prompt lines describing how to behave for the given profile choices.

    Unknown values are skipped silently — the catalog only covers the curated
    options exposed by the dashboard, and new/unrecognized labels should not
    break prompt construction.
    """
    guidance: list[str] = []
    mode_line = PERSONA_MODE_GUIDANCE.get(persona_mode)
    if mode_line:
        guidance.append(f"- {mode_line}")
    style_line = INTERACTION_STYLE_GUIDANCE.get(interaction_style)
    if style_line:
        guidance.append(f"- {style_line}")

    if not guidance:
        return []
    return ["Como se comportar com este usuario:", *guidance]
