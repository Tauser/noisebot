"""bridgev2.runtime.orchestrator -- Maestro do event loop.

O Orchestrator e o unico componente que cruza fronteiras de dominio.
Ele:
  - assina o bus para todos os eventos relevantes
  - delega ao TurnManager as transicoes de estado
  - chama os providers em sequencia (Intent -> Robot; STT/LLM/TTS nas proximas fases)
  - gerencia a Task de turno (cancelada no barge-in -- Invariante I-5)

Fase 1: esqueleto que sobe o loop, processa eventos de conexao, loga.
Fase 2: recebe get_adapter para enviar comandos ao firmware.
Fase 3: FinalTranscript → LocalIntentProvider → RobotOutputProvider → FSM completa.
Fase 4: STT real (faster-whisper) no COMMITTING_TURN.
"""
from __future__ import annotations

import asyncio
import logging
from typing import Any, Callable

from .bus import EventBus
from .events import (
    FirmwareConnected,
    FirmwareDisconnected,
    ShutdownRequested,
    VoiceActivityStart,
    VoiceActivityEnd,
    AudioChunkIn,
    WakeDetected,
    StatusUpdate,
    BargeInDetected,
    TurnError,
    FinalTranscript,
    IntentResolved,
    SpeechDone,
    RobotCommand,
)
from .session import SessionContext, new_turn_id
from .turn_manager import TurnManager, TurnState
from ..llm.local_intent import LocalIntentProvider
from ..robot.output import RobotOutputProvider

log = logging.getLogger(__name__)

TURN_DEADLINE_S = 30.0  # watchdog: turno maximo em segundos


class Orchestrator:
    """Coordena o pipeline de voz sobre o event loop asyncio.

    get_adapter: callable() -> FirmwareAdapter | None
        Injetado pelo Application para acesso ao adapter ativo.
        None significa sem transporte (modo dry-run / headless).
    """

    def __init__(
        self,
        bus: EventBus,
        config: Any = None,
        get_adapter: Callable[[], Any] | None = None,
    ) -> None:
        self._bus = bus
        self._config = config
        self._get_adapter = get_adapter or (lambda: None)
        self._fsm = TurnManager()
        self._session: SessionContext | None = None
        self._turn_task: asyncio.Task | None = None
        self._running = False

        # Providers Fase 3
        self._intent = LocalIntentProvider()
        self._robot = RobotOutputProvider(bus)

        # Queue de eventos: o Orchestrator assina todos
        self._events = bus.subscribe(maxsize=-1)  # ilimitado para o maestro

    @property
    def adapter(self):
        """Acesso ao FirmwareAdapter ativo (ou None)."""
        return self._get_adapter()

    # -- Ciclo principal ----------------------------------------------------

    async def run(self) -> None:
        """Loop principal -- processa eventos do bus ate ShutdownRequested."""
        self._running = True
        log.info("Orchestrator: iniciando loop (FSM em %s)", self._fsm.state.name)

        async for event in EventBus.iter_queue(self._events):
            try:
                await self._dispatch(event)
            except Exception:
                log.exception("Orchestrator: erro ao processar %s", type(event).__name__)

        log.info("Orchestrator: loop encerrado")

    async def shutdown(self) -> None:
        """Encerra o orchestrator graciosamente."""
        self._running = False
        if self._turn_task and not self._turn_task.done():
            self._turn_task.cancel()
            try:
                await self._turn_task
            except asyncio.CancelledError:
                pass
        self._fsm.reset_to_idle()
        await self._bus.close()

    # -- Dispatch de eventos ------------------------------------------------

    async def _dispatch(self, event: Any) -> None:
        match event:
            case FirmwareConnected():
                await self._on_firmware_connected(event)
            case FirmwareDisconnected():
                await self._on_firmware_disconnected(event)
            case WakeDetected():
                await self._on_wake(event)
            case VoiceActivityStart():
                await self._on_voice_start(event)
            case AudioChunkIn():
                await self._on_audio_chunk(event)
            case VoiceActivityEnd():
                await self._on_voice_end(event)
            case FinalTranscript():
                await self._on_final_transcript(event)
            case StatusUpdate():
                log.debug("StatusUpdate: state=%d valence=%.2f", event.state, event.valence)
            case BargeInDetected():
                await self._on_barge_in(event)
            case TurnError():
                await self._on_turn_error(event)
            case ShutdownRequested():
                await self.shutdown()
            case RobotCommand():
                pass  # publicados pelo RobotOutputProvider -- ja no bus, nao re-despachar
            case SpeechDone():
                await self._on_speech_done(event)
            case _:
                log.debug("Orchestrator: evento nao tratado %s", type(event).__name__)

    # -- Handlers de estado -------------------------------------------------

    async def _on_firmware_connected(self, event: FirmwareConnected) -> None:
        log.info("Firmware conectado. capabilities=%s", event.peer_capabilities.get("features", []))
        self._fsm.reset_to_idle()

    async def _on_firmware_disconnected(self, event: FirmwareDisconnected) -> None:
        log.warning("Firmware desconectado: %s", event.reason)
        await self._cancel_current_turn(reason="transport_disconnected")
        self._fsm.reset_to_idle()

    async def _on_wake(self, event: WakeDetected) -> None:
        if not self._fsm.is_idle:
            log.debug("Wake detectado fora de IDLE (estado=%s) -- ignorado", self._fsm.state.name)
            return
        await self._begin_turn()

    async def _on_voice_start(self, event: VoiceActivityStart) -> None:
        if self._fsm.is_idle:
            await self._begin_turn()
        elif self._fsm.can_interrupt:
            await self._bus.publish(BargeInDetected(turn_id=self._fsm.current_turn_id))

    async def _on_audio_chunk(self, event: AudioChunkIn) -> None:
        if self._session and self._fsm.is_listening:
            self._session.append_audio(event.pcm)

    async def _on_voice_end(self, event: VoiceActivityEnd) -> None:
        if not self._fsm.is_listening:
            return
        session = self._session
        if session is None:
            self._fsm.try_transition(TurnState.IDLE)
            return

        # Valida se ha audio suficiente
        if session.total_samples < 8000:  # < 500 ms
            session.discard_reason = "audio_curto"
            log.debug("Turno %d descartado: %s", session.turn_id, session.discard_reason)
            self._fsm.try_transition(TurnState.IDLE)
            await self._finish_turn()
            return

        session.mark("voice_end")
        self._fsm.transition(TurnState.COMMITTING_TURN, turn_id=session.turn_id)

        # Fase 3: sem STT -- aguarda FinalTranscript injetado externamente
        # Fase 4 substitui este bloco por whisper em worker
        log.info(
            "Turno %d comprometido: %d amostras (%.1f s) -- aguardando FinalTranscript (Fase 4)",
            session.turn_id,
            session.total_samples,
            session.duration_s(),
        )

    async def _on_final_transcript(self, event: FinalTranscript) -> None:
        """Fase 3: FinalTranscript (sintetico ou de STT) → intent → robot.

        Pode chegar de:
          - Injecao direta no bus (testes / debug CLI)
          - STT real (Fase 4)
        """
        # Garante que o turn_id bate (ou cria sessao sintetica se chegou do bus direto)
        if self._session is None or self._session.turn_id != event.turn_id:
            if self._fsm.is_idle:
                # Injecao direta sem sessao de audio -- aceita como turno sintetico
                self._session = SessionContext(turn_id=event.turn_id)
                self._fsm.transition(TurnState.LISTENING, turn_id=event.turn_id)
                self._fsm.transition(TurnState.COMMITTING_TURN, turn_id=event.turn_id)
            else:
                log.debug(
                    "FinalTranscript turn_id=%d ignorado (sessao ativa=%s)",
                    event.turn_id,
                    self._session.turn_id if self._session else "None",
                )
                return

        session = self._session
        session.final_text = event.text
        session.mark("final_transcript")

        if not event.is_usable:
            log.debug("Turno %d: transcript nao utilizavel (%s)", event.turn_id, event.quality.name)
            self._fsm.try_transition(TurnState.IDLE)
            await self._finish_turn()
            return

        # COMMITTING_TURN → THINKING
        if self._fsm.state != TurnState.COMMITTING_TURN:
            self._fsm.try_transition(TurnState.COMMITTING_TURN, turn_id=event.turn_id)
        self._fsm.transition(TurnState.THINKING, turn_id=event.turn_id)
        session.mark("thinking_start")

        # Resolve intent local
        context = {"status": {}}
        intent = self._intent.match(
            text=event.text,
            turn_id=event.turn_id,
            context=context,
        )
        session.intent_name = intent.intent_name
        if intent.reply_text:
            session.reply_text = intent.reply_text

        # Publica IntentResolved no bus
        await self._bus.publish(intent)
        session.mark("intent_resolved")

        if intent.has_intent:
            log.info(
                "Turno %d: intent=%s reply=%r",
                event.turn_id, intent.intent_name, intent.reply_text,
            )
            # THINKING → SPEAKING
            self._fsm.transition(TurnState.SPEAKING, turn_id=event.turn_id)
            session.mark("speaking_start")

            # Emite comandos de robot
            await self._robot.emit_for_intent(intent, self.adapter)

            # Fase 3: sem audio → SpeechDone imediato
            await self._bus.publish(SpeechDone(turn_id=event.turn_id))

        else:
            # Sem intent local → iria para LLM (Fase 5); por enquanto loga e encerra
            log.info(
                "Turno %d: sem intent local para %r -- iria a LLM (Fase 5)",
                event.turn_id, event.text,
            )
            self._fsm.try_transition(TurnState.IDLE)
            await self._finish_turn()

    async def _on_speech_done(self, event: SpeechDone) -> None:
        """Turno de fala encerrado → volta a IDLE com baseline."""
        if self._session and self._session.turn_id != event.turn_id:
            return
        session = self._session
        if session:
            session.mark("speech_done")

        # Restaura baseline antes de IDLE (regra do CLAUDE.md)
        await self._robot.reset_baseline(self.adapter, event.turn_id)

        self._fsm.try_transition(TurnState.IDLE)
        await self._finish_turn()

    async def _on_barge_in(self, event: BargeInDetected) -> None:
        if not self._fsm.can_interrupt:
            return
        log.info("Barge-in no turno %d", event.turn_id)
        adapter = self.adapter
        if adapter is not None:
            await adapter.send_speech_cancel(event.turn_id)
        await self._cancel_current_turn(reason="barge_in")
        self._fsm.try_transition(TurnState.INTERRUPTED)
        self._fsm.try_transition(TurnState.LISTENING)
        self._session = SessionContext(turn_id=new_turn_id())

    async def _on_turn_error(self, event: TurnError) -> None:
        log.error("Erro no turno %d estagio=%s: %s", event.turn_id, event.stage, event.reason)
        await self._cancel_current_turn(reason=f"error:{event.stage}")
        self._fsm.try_transition(TurnState.ERROR_RECOVERY)
        self._fsm.try_transition(TurnState.IDLE)
        await self._finish_turn()

    # -- Helpers internos ---------------------------------------------------

    async def _begin_turn(self) -> None:
        """Inicia um novo turno: cria sessao e transiciona para LISTENING."""
        self._session = SessionContext(turn_id=new_turn_id())
        self._session.set_deadline(TURN_DEADLINE_S)
        self._fsm.transition(TurnState.LISTENING, turn_id=self._session.turn_id)
        log.info("Turno %d iniciado (LISTENING)", self._session.turn_id)

    async def _cancel_current_turn(self, reason: str = "") -> None:
        """Cancela a Task de turno atual se existir."""
        if self._turn_task and not self._turn_task.done():
            self._turn_task.cancel()
            try:
                await asyncio.wait_for(asyncio.shield(self._turn_task), timeout=0.5)
            except (asyncio.CancelledError, asyncio.TimeoutError):
                pass
        if self._session:
            self._session.discard_reason = reason or "cancelled"

    async def _finish_turn(self) -> None:
        """Limpa estado de sessao apos o turno terminar."""
        if self._session:
            log.debug("Turno %d finalizado: %s", self._session.turn_id, self._session.to_log_dict())
        self._session = None
        self._turn_task = None
