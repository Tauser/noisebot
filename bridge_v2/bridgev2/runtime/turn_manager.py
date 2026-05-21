"""bridgev2.runtime.turn_manager — FSM de turno half-duplex.

Estados (§5 do BRIDGE_V2.md):
  IDLE → LISTENING → COMMITTING_TURN → THINKING → SPEAKING → IDLE
                                                 ↘ ERROR_RECOVERY
                                  INTERRUPTED ←──┘ (barge-in)

Invariantes:
  I-1: SAY só é emitido em SPEAKING.
  I-2: THINKING/SPEAKING só alcançados via COMMITTING_TURN.
  I-3: turn_id monotônico; Output Scheduler descarta turnos obsoletos.
  I-4: todo turno termina em IDLE dentro do deadline (watchdog).
  I-5: só uma Task de turno existe por vez.

Regra de baseline (alinhada ao CLAUDE.md):
  Toda transição para IDLE limpa expressão, gaze, postura e overlays.
"""
from __future__ import annotations

import logging
from enum import Enum, auto

log = logging.getLogger(__name__)


class TurnState(Enum):
    IDLE = auto()
    LISTENING = auto()
    COMMITTING_TURN = auto()
    THINKING = auto()
    SPEAKING = auto()
    INTERRUPTED = auto()
    ERROR_RECOVERY = auto()


# Transições válidas: estado atual → conjunto de próximos estados permitidos
_VALID_TRANSITIONS: dict[TurnState, frozenset[TurnState]] = {
    TurnState.IDLE: frozenset({
        TurnState.LISTENING,
    }),
    TurnState.LISTENING: frozenset({
        TurnState.LISTENING,        # AudioChunkIn (self-loop)
        TurnState.COMMITTING_TURN,  # VoiceActivityEnd com áudio
        TurnState.IDLE,             # VoiceActivityEnd sem fala real (falso positivo)
        TurnState.INTERRUPTED,      # barge-in enquanto escuta (raro, mas possível)
        TurnState.ERROR_RECOVERY,
    }),
    TurnState.COMMITTING_TURN: frozenset({
        TurnState.THINKING,         # FinalTranscript plausível
        TurnState.IDLE,             # transcript vazio / descartado
        TurnState.ERROR_RECOVERY,
    }),
    TurnState.THINKING: frozenset({
        TurnState.SPEAKING,         # resposta pronta
        TurnState.IDLE,             # sem intent local (Fase 3 stub) / sem LLM reply
        TurnState.INTERRUPTED,      # barge-in
        TurnState.ERROR_RECOVERY,
    }),
    TurnState.SPEAKING: frozenset({
        TurnState.SPEAKING,         # TtsAudioChunk (self-loop)
        TurnState.IDLE,             # SpeechDone (fim natural)
        TurnState.INTERRUPTED,      # barge-in
        TurnState.ERROR_RECOVERY,
    }),
    TurnState.INTERRUPTED: frozenset({
        TurnState.LISTENING,        # cancelamento concluído → novo turno
        TurnState.IDLE,             # cancelamento sem nova fala
        TurnState.ERROR_RECOVERY,
    }),
    TurnState.ERROR_RECOVERY: frozenset({
        TurnState.IDLE,             # baseline restaurado
    }),
}


class TurnManager:
    """Gerencia transições de estado do turno e aplica as regras de turn-taking.

    Emite callbacks opcionais `on_enter_<state>` e `on_exit_<state>` se definidos
    em subclasse ou atribuídos externamente.
    """

    def __init__(self) -> None:
        self._state = TurnState.IDLE
        self._current_turn_id: int = 0

    @property
    def state(self) -> TurnState:
        return self._state

    @property
    def current_turn_id(self) -> int:
        return self._current_turn_id

    @property
    def is_idle(self) -> bool:
        return self._state == TurnState.IDLE

    @property
    def is_listening(self) -> bool:
        return self._state == TurnState.LISTENING

    @property
    def can_speak(self) -> bool:
        """True somente se estiver em SPEAKING (Invariante I-1)."""
        return self._state == TurnState.SPEAKING

    @property
    def can_interrupt(self) -> bool:
        return self._state in (TurnState.THINKING, TurnState.SPEAKING)

    def transition(self, new_state: TurnState, turn_id: int | None = None) -> None:
        """Executa transição de estado.

        Lança ValueError se a transição for inválida.
        Atualiza current_turn_id quando fornecido.
        """
        allowed = _VALID_TRANSITIONS.get(self._state, frozenset())
        if new_state not in allowed:
            raise ValueError(
                f"Transição inválida: {self._state.name} → {new_state.name}"
            )

        old_state = self._state
        if turn_id is not None:
            self._current_turn_id = turn_id

        # callbacks de saída (opcionais)
        exit_cb = getattr(self, f"on_exit_{old_state.name.lower()}", None)
        if callable(exit_cb):
            exit_cb()

        self._state = new_state
        log.debug(
            "FSM: %s → %s (turn_id=%d)",
            old_state.name,
            new_state.name,
            self._current_turn_id,
        )

        # callbacks de entrada (opcionais)
        enter_cb = getattr(self, f"on_enter_{new_state.name.lower()}", None)
        if callable(enter_cb):
            enter_cb()

    def try_transition(self, new_state: TurnState, turn_id: int | None = None) -> bool:
        """Tenta transição sem lançar exceção. Retorna True se bem-sucedida."""
        try:
            self.transition(new_state, turn_id)
            return True
        except ValueError:
            log.warning(
                "FSM: transição ignorada %s → %s",
                self._state.name,
                new_state.name,
            )
            return False

    def reset_to_idle(self) -> None:
        """Força reset para IDLE independentemente do estado atual (uso em shutdown/recovery)."""
        old = self._state
        self._state = TurnState.IDLE
        log.debug("FSM: reset forçado %s → IDLE", old.name)
