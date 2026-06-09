"""Translate raw VAA floats (valence/activation/attention/trust) to natural language.

The LLM never receives raw floats — only the textual summary produced here.
"""
from __future__ import annotations


def describe_mood(
    valence: float = 0.0,
    activation: float = 0.0,
    attention: float = 0.0,
    trust: float = 0.0,
) -> str:
    """Return a short Portuguese mood description suitable for LLM context.

    Rules (applied in order, first match wins for the base):
      valence > 0.5  and activation > 0.5   → "animado e receptivo"
      valence > 0.5  and activation ≤ 0.5   → "calmo e contente"
      valence < -0.4 and activation > 0.5   → "irritado ou alarmado"
      valence < -0.4 and activation ≤ 0.5   → "triste ou retraído"
      otherwise                              → "neutro"

    Modifier appended with " + " when:
      attention > 0.7  → "focado no usuário"
      trust > 0.7      → "familiaridade alta"
    """
    if valence > 0.5 and activation > 0.5:
        base = "animado e receptivo"
    elif valence > 0.5 and activation <= 0.5:
        base = "calmo e contente"
    elif valence < -0.4 and activation > 0.5:
        base = "irritado ou alarmado"
    elif valence < -0.4 and activation <= 0.5:
        base = "triste ou retraído"
    else:
        base = "neutro"

    modifiers: list[str] = []
    if attention > 0.7:
        modifiers.append("focado no usuário")
    if trust > 0.7:
        modifiers.append("familiaridade alta")

    if modifiers:
        return base + " + " + " + ".join(modifiers)
    return base


__all__ = ["describe_mood"]
