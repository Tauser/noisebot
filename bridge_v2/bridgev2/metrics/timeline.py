"""bridgev2.metrics.timeline — Cronômetro de turno com os 12 marcos (Fase 4)."""
from __future__ import annotations
import time
from dataclasses import dataclass, field


MARKERS = [
    "wake_to_listen_ms",
    "audio_end_to_stt_start_ms",
    "stt_ms",
    "end_of_turn_ms",
    "local_intent_ms",
    "llm_first_token_ms",
    "llm_total_ms",
    "tts_first_audio_ms",
    "tts_total_ms",
    "first_robot_reaction_ms",
    "first_audio_out_ms",
    "interruption_cancel_ms",
]


@dataclass
class TurnTimeline:
    turn_id: int
    _marks: dict[str, float] = field(default_factory=dict)

    def mark(self, name: str) -> None:
        self._marks[name] = time.monotonic()

    def elapsed_ms(self, start: str, end: str) -> float | None:
        t0 = self._marks.get(start)
        t1 = self._marks.get(end)
        if t0 is None or t1 is None:
            return None
        return (t1 - t0) * 1000.0

    def to_dict(self) -> dict[str, float]:
        return dict(self._marks)
