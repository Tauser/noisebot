"""bridgev2.robot.output -- RobotOutputProvider: decisao → comandos firmware (Fase 3).

Traduz IntentResolved em RobotCommand eventos no bus e chama o adapter
diretamente para EXPR / ACTION / EMOT_EVENT / GAZE / TEXT_SCROLL.

Regras de safety preservadas:
  - Nenhum comando de posicao de servo emitido aqui.
  - Toda movimentacao fisica continua mediada por motion_safety no firmware.
  - Apenas ACTION/GAZE pre-validados pelo firmware sao emitidos.
"""
from __future__ import annotations

import asyncio
import logging
from typing import Any

from ..runtime.bus import EventBus
from ..runtime.events import IntentResolved, RobotCommand

log = logging.getLogger(__name__)

# Gaze por intent de direcao
_GAZE_MAP: dict[str, tuple[float, float]] = {
    "local_look_left":  (-0.75, 0.0),
    "local_look_right": ( 0.75, 0.0),
    "local_look_up":    ( 0.0, -0.55),
    "local_look_down":  ( 0.0,  0.55),
}


class RobotOutputProvider:
    """Traduz decisoes do Orchestrator em comandos para o firmware.

    Fase 3: apenas EXPR / ACTION / EMOT_EVENT / GAZE.
    Fase 6 adiciona SAY (TTS → SayChunkOut).
    """

    def __init__(self, bus: EventBus) -> None:
        self._bus = bus

    async def emit_for_intent(
        self,
        intent: IntentResolved,
        adapter: Any,   # FirmwareAdapter | None
        include_reply_text: bool = True,
    ) -> None:
        """Emite comandos de robot baseados em IntentResolved.

        Publica RobotCommand no bus para observabilidade.
        Chama adapter diretamente se disponivel.
        """
        if not intent.has_intent:
            return

        # -- Expressao facial --------------------------------------------------
        if intent.expression_id is not None:
            await self._emit(
                adapter, "expr",
                {"expression_id": intent.expression_id, "duration_ms": 3000},
                intent.turn_id,
            )

        # -- Evento emocional --------------------------------------------------
        if intent.emot_event_id is not None:
            await self._emit(
                adapter, "emot",
                {"event_id": intent.emot_event_id},
                intent.turn_id,
            )

        # -- Acao corporal -----------------------------------------------------
        if intent.action_id is not None and intent.action_id != 0:
            await self._emit(
                adapter, "action",
                {"action_id": intent.action_id},
                intent.turn_id,
            )

        # -- Gaze (direcao especifica) -----------------------------------------
        gaze = _GAZE_MAP.get(intent.intent_name or "")
        if gaze:
            await self._emit(
                adapter, "gaze",
                {"x": gaze[0], "y": gaze[1]},
                intent.turn_id,
            )

        # -- Comandos locais precisam chegar antes de qualquer reply/TTS. --------
        if intent.device_command:
            await self._emit(
                adapter, "session",
                intent.device_command,
                intent.turn_id,
            )

        # -- Texto de reply no display -----------------------------------------
        if include_reply_text and intent.reply_text:
            await self._emit(
                adapter, "text",
                {"text": intent.reply_text[:128]},
                intent.turn_id,
            )

    async def reset_baseline(self, adapter: Any, turn_id: int = 0) -> None:
        """Restaura baseline IDLE: NEUTRAL + gaze central.

        Chamado ao entrar em IDLE — alinhado a regra de baseline do CLAUDE.md.
        """
        await self._emit(
            adapter, "expr",
            {"expression_id": 2, "duration_ms": 500},  # NEUTRAL
            turn_id,
        )
        await self._emit(
            adapter, "gaze",
            {"x": 0.0, "y": 0.0},
            turn_id,
        )

    async def _emit(
        self,
        adapter: Any,
        kind: str,
        payload: dict,
        turn_id: int,
    ) -> None:
        """Publica RobotCommand no bus e despacha para o adapter."""
        cmd = RobotCommand(kind=kind, payload=payload, turn_id=turn_id)
        await self._bus.publish(cmd)

        if adapter is None:
            return

        try:
            if kind == "expr":
                await adapter.send_expr(
                    payload["expression_id"],
                    payload.get("duration_ms", 2000),
                )
            elif kind == "action":
                await adapter.send_action(payload["action_id"])
            elif kind == "emot":
                await adapter.send_emot_event(payload["event_id"])
            elif kind == "gaze":
                await adapter.send_gaze(payload["x"], payload["y"])
            elif kind == "text":
                await adapter.send_text_scroll(payload["text"])
            elif kind == "volume":
                await adapter.send_volume(payload.get("percent", 50))
            elif kind == "session":
                await adapter.send_session(payload)
        except Exception:
            log.exception("RobotOutputProvider: erro ao enviar %s turn_id=%d", kind, turn_id)
