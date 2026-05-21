"""Testes unitários: TurnManager — FSM de turno half-duplex.

Cobre:
  - Transições válidas para cada estado
  - Transições inválidas lançam ValueError
  - try_transition retorna False (sem exceção) em transição inválida
  - reset_to_idle() força IDLE de qualquer estado
  - turn_id atualizado em transition(); preservado em try_transition falha
  - Propriedades booleanas (is_idle, is_listening, can_speak, can_interrupt)
  - Callbacks on_enter_* e on_exit_* chamados na ordem certa
  - Invariante I-2: THINKING/SPEAKING só via COMMITTING_TURN
"""
from __future__ import annotations

import pytest

from bridgev2.runtime.turn_manager import TurnManager, TurnState


# ── Helpers ──────────────────────────────────────────────────────────────────

def _fsm_at(state: TurnState, turn_id: int = 1) -> TurnManager:
    """Constrói um TurnManager forçado no estado desejado."""
    fsm = TurnManager()
    fsm.reset_to_idle()
    # usa reset + atribuição interna para testes de estados avançados
    fsm._state = state
    fsm._current_turn_id = turn_id
    return fsm


# ── Estado inicial ────────────────────────────────────────────────────────────

class TestInitialState:
    def test_starts_idle(self):
        fsm = TurnManager()
        assert fsm.state == TurnState.IDLE

    def test_initial_turn_id_zero(self):
        fsm = TurnManager()
        assert fsm.current_turn_id == 0

    def test_is_idle_true_initially(self):
        fsm = TurnManager()
        assert fsm.is_idle is True

    def test_is_listening_false_initially(self):
        fsm = TurnManager()
        assert fsm.is_listening is False

    def test_can_speak_false_initially(self):
        fsm = TurnManager()
        assert fsm.can_speak is False

    def test_can_interrupt_false_initially(self):
        fsm = TurnManager()
        assert fsm.can_interrupt is False


# ── Transições válidas ────────────────────────────────────────────────────────

class TestValidTransitions:
    def test_idle_to_listening(self):
        fsm = TurnManager()
        fsm.transition(TurnState.LISTENING, turn_id=1)
        assert fsm.state == TurnState.LISTENING
        assert fsm.current_turn_id == 1

    def test_listening_to_committing(self):
        fsm = _fsm_at(TurnState.LISTENING, 1)
        fsm.transition(TurnState.COMMITTING_TURN)
        assert fsm.state == TurnState.COMMITTING_TURN

    def test_listening_to_idle(self):
        fsm = _fsm_at(TurnState.LISTENING, 1)
        fsm.transition(TurnState.IDLE)
        assert fsm.state == TurnState.IDLE

    def test_listening_self_loop(self):
        fsm = _fsm_at(TurnState.LISTENING, 1)
        fsm.transition(TurnState.LISTENING)
        assert fsm.state == TurnState.LISTENING

    def test_committing_to_thinking(self):
        fsm = _fsm_at(TurnState.COMMITTING_TURN, 2)
        fsm.transition(TurnState.THINKING)
        assert fsm.state == TurnState.THINKING

    def test_committing_to_idle(self):
        fsm = _fsm_at(TurnState.COMMITTING_TURN, 2)
        fsm.transition(TurnState.IDLE)
        assert fsm.state == TurnState.IDLE

    def test_thinking_to_speaking(self):
        fsm = _fsm_at(TurnState.THINKING, 3)
        fsm.transition(TurnState.SPEAKING)
        assert fsm.state == TurnState.SPEAKING

    def test_thinking_to_interrupted(self):
        fsm = _fsm_at(TurnState.THINKING, 3)
        fsm.transition(TurnState.INTERRUPTED)
        assert fsm.state == TurnState.INTERRUPTED

    def test_speaking_to_idle(self):
        fsm = _fsm_at(TurnState.SPEAKING, 4)
        fsm.transition(TurnState.IDLE)
        assert fsm.state == TurnState.IDLE

    def test_speaking_self_loop(self):
        fsm = _fsm_at(TurnState.SPEAKING, 4)
        fsm.transition(TurnState.SPEAKING)
        assert fsm.state == TurnState.SPEAKING

    def test_speaking_to_interrupted(self):
        fsm = _fsm_at(TurnState.SPEAKING, 4)
        fsm.transition(TurnState.INTERRUPTED)
        assert fsm.state == TurnState.INTERRUPTED

    def test_interrupted_to_listening(self):
        fsm = _fsm_at(TurnState.INTERRUPTED, 5)
        fsm.transition(TurnState.LISTENING, turn_id=6)
        assert fsm.state == TurnState.LISTENING
        assert fsm.current_turn_id == 6

    def test_interrupted_to_idle(self):
        fsm = _fsm_at(TurnState.INTERRUPTED, 5)
        fsm.transition(TurnState.IDLE)
        assert fsm.state == TurnState.IDLE

    def test_error_recovery_to_idle(self):
        fsm = _fsm_at(TurnState.ERROR_RECOVERY, 7)
        fsm.transition(TurnState.IDLE)
        assert fsm.state == TurnState.IDLE


# ── Transições inválidas ──────────────────────────────────────────────────────

class TestInvalidTransitions:
    def test_idle_to_thinking_raises(self):
        """Invariante I-2: THINKING só via COMMITTING_TURN."""
        fsm = TurnManager()
        with pytest.raises(ValueError, match="IDLE → THINKING"):
            fsm.transition(TurnState.THINKING)

    def test_idle_to_speaking_raises(self):
        """Invariante I-2: SPEAKING só via COMMITTING_TURN."""
        fsm = TurnManager()
        with pytest.raises(ValueError, match="IDLE → SPEAKING"):
            fsm.transition(TurnState.SPEAKING)

    def test_idle_to_committing_raises(self):
        """IDLE → COMMITTING_TURN sem passar por LISTENING é inválido."""
        fsm = TurnManager()
        with pytest.raises(ValueError):
            fsm.transition(TurnState.COMMITTING_TURN)

    def test_listening_to_thinking_raises(self):
        """Deve passar por COMMITTING_TURN antes."""
        fsm = _fsm_at(TurnState.LISTENING)
        with pytest.raises(ValueError, match="LISTENING → THINKING"):
            fsm.transition(TurnState.THINKING)

    def test_listening_to_speaking_raises(self):
        fsm = _fsm_at(TurnState.LISTENING)
        with pytest.raises(ValueError, match="LISTENING → SPEAKING"):
            fsm.transition(TurnState.SPEAKING)

    def test_committing_to_committing_raises(self):
        fsm = _fsm_at(TurnState.COMMITTING_TURN)
        with pytest.raises(ValueError):
            fsm.transition(TurnState.COMMITTING_TURN)

    def test_thinking_to_idle_is_valid(self):
        """THINKING → IDLE é permitido para o path sem LLM (Fase 3 stub / sem intent)."""
        fsm = _fsm_at(TurnState.THINKING)
        fsm.transition(TurnState.IDLE)   # deve funcionar sem exceção
        assert fsm.state == TurnState.IDLE

    def test_error_recovery_to_listening_raises(self):
        fsm = _fsm_at(TurnState.ERROR_RECOVERY)
        with pytest.raises(ValueError):
            fsm.transition(TurnState.LISTENING)


# ── try_transition ────────────────────────────────────────────────────────────

class TestTryTransition:
    def test_valid_returns_true(self):
        fsm = TurnManager()
        result = fsm.try_transition(TurnState.LISTENING, turn_id=1)
        assert result is True
        assert fsm.state == TurnState.LISTENING

    def test_invalid_returns_false(self):
        fsm = TurnManager()
        result = fsm.try_transition(TurnState.SPEAKING)
        assert result is False
        assert fsm.state == TurnState.IDLE  # estado não mudou

    def test_invalid_does_not_change_turn_id(self):
        fsm = _fsm_at(TurnState.IDLE, turn_id=42)
        fsm.try_transition(TurnState.SPEAKING, turn_id=99)
        assert fsm.current_turn_id == 42  # turn_id preservado na falha


# ── reset_to_idle ─────────────────────────────────────────────────────────────

class TestResetToIdle:
    def test_reset_from_listening(self):
        fsm = _fsm_at(TurnState.LISTENING)
        fsm.reset_to_idle()
        assert fsm.state == TurnState.IDLE

    def test_reset_from_thinking(self):
        fsm = _fsm_at(TurnState.THINKING)
        fsm.reset_to_idle()
        assert fsm.state == TurnState.IDLE

    def test_reset_from_speaking(self):
        fsm = _fsm_at(TurnState.SPEAKING)
        fsm.reset_to_idle()
        assert fsm.state == TurnState.IDLE

    def test_reset_from_error_recovery(self):
        fsm = _fsm_at(TurnState.ERROR_RECOVERY)
        fsm.reset_to_idle()
        assert fsm.state == TurnState.IDLE

    def test_reset_from_idle_is_noop(self):
        fsm = TurnManager()
        fsm.reset_to_idle()
        assert fsm.state == TurnState.IDLE


# ── Propriedades booleanas ────────────────────────────────────────────────────

class TestBooleanProperties:
    def test_is_idle(self):
        fsm = TurnManager()
        assert fsm.is_idle is True
        fsm.transition(TurnState.LISTENING, turn_id=1)
        assert fsm.is_idle is False

    def test_is_listening(self):
        fsm = _fsm_at(TurnState.LISTENING)
        assert fsm.is_listening is True
        fsm.transition(TurnState.COMMITTING_TURN)
        assert fsm.is_listening is False

    def test_can_speak_only_in_speaking(self):
        for state in TurnState:
            fsm = _fsm_at(state)
            expected = (state == TurnState.SPEAKING)
            assert fsm.can_speak is expected, f"can_speak deveria ser {expected} em {state.name}"

    def test_can_interrupt_in_thinking_and_speaking(self):
        for state in TurnState:
            fsm = _fsm_at(state)
            expected = state in (TurnState.THINKING, TurnState.SPEAKING)
            assert fsm.can_interrupt is expected, (
                f"can_interrupt deveria ser {expected} em {state.name}"
            )


# ── turn_id monotônico (Invariante I-3) ───────────────────────────────────────

class TestTurnIdMonotonic:
    def test_turn_id_updated_on_transition(self):
        fsm = TurnManager()
        fsm.transition(TurnState.LISTENING, turn_id=10)
        assert fsm.current_turn_id == 10
        fsm.transition(TurnState.COMMITTING_TURN, turn_id=11)
        assert fsm.current_turn_id == 11

    def test_turn_id_not_updated_when_none(self):
        fsm = TurnManager()
        fsm.transition(TurnState.LISTENING, turn_id=5)
        fsm.transition(TurnState.COMMITTING_TURN)  # sem turn_id
        assert fsm.current_turn_id == 5


# ── Callbacks on_enter / on_exit ─────────────────────────────────────────────

class TestCallbacks:
    def test_on_enter_called(self):
        fsm = TurnManager()
        entered = []
        fsm.on_enter_listening = lambda: entered.append("listening")
        fsm.transition(TurnState.LISTENING, turn_id=1)
        assert entered == ["listening"]

    def test_on_exit_called(self):
        fsm = TurnManager()
        exited = []
        fsm.on_exit_idle = lambda: exited.append("idle")
        fsm.transition(TurnState.LISTENING, turn_id=1)
        assert exited == ["idle"]

    def test_exit_before_enter(self):
        """on_exit do estado antigo é chamado ANTES de on_enter do novo."""
        fsm = TurnManager()
        order = []
        fsm.on_exit_idle = lambda: order.append("exit_idle")
        fsm.on_enter_listening = lambda: order.append("enter_listening")
        fsm.transition(TurnState.LISTENING, turn_id=1)
        assert order == ["exit_idle", "enter_listening"]


# ── Fluxo completo IDLE→...→IDLE ─────────────────────────────────────────────

class TestFullTurnFlow:
    def test_complete_happy_path(self):
        """IDLE → LISTENING → COMMITTING → THINKING → SPEAKING → IDLE."""
        fsm = TurnManager()
        fsm.transition(TurnState.LISTENING, turn_id=1)
        assert fsm.is_listening
        fsm.transition(TurnState.COMMITTING_TURN)
        assert fsm.state == TurnState.COMMITTING_TURN
        fsm.transition(TurnState.THINKING)
        assert not fsm.can_speak
        assert fsm.can_interrupt
        fsm.transition(TurnState.SPEAKING)
        assert fsm.can_speak
        fsm.transition(TurnState.IDLE)
        assert fsm.is_idle

    def test_barge_in_path(self):
        """SPEAKING → INTERRUPTED → LISTENING (novo turno)."""
        fsm = _fsm_at(TurnState.SPEAKING, turn_id=1)
        fsm.transition(TurnState.INTERRUPTED)
        fsm.transition(TurnState.LISTENING, turn_id=2)
        assert fsm.is_listening
        assert fsm.current_turn_id == 2

    def test_short_audio_path(self):
        """LISTENING → IDLE (áudio curto, descartado)."""
        fsm = TurnManager()
        fsm.transition(TurnState.LISTENING, turn_id=1)
        fsm.transition(TurnState.IDLE)
        assert fsm.is_idle
