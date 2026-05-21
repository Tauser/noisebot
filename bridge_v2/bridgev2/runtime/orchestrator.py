"""bridgev2.runtime.orchestrator -- Maestro do event loop.

O Orchestrator é o único componente que cruza fronteiras de domínio.
Ele:
  - assina o bus para todos os eventos relevantes
  - delega ao TurnManager as transições de estado
  - chama os providers em sequência (STT → Intent → Robot; LLM/TTS nas próximas fases)
  - gerencia a Task de turno (cancelada no barge-in — Invariante I-5)

Fase 1: esqueleto que sobe o loop, processa eventos de conexão, loga.
Fase 2: recebe get_adapter para enviar comandos ao firmware.
Fase 3: FinalTranscript → LocalIntentProvider → RobotOutputProvider → FSM completa.
Fase 4: STT real (faster-whisper) no COMMITTING_TURN via run_in_executor.
         Métricas: stt_ms, audio_end_to_stt_start_ms, end_of_turn_ms.
         STTProvider=None → continua aceitando FinalTranscript sintético (Fase 3 compat).
"""
from __future__ import annotations

import asyncio
import logging
import time
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
from ..metrics.registry import MetricsRegistry

log = logging.getLogger(__name__)

TURN_DEADLINE_S = 30.0  # watchdog: turno máximo em segundos


class Orchestrator:
    """Coordena o pipeline de voz sobre o event loop asyncio.

    get_adapter: callable() -> FirmwareAdapter | None
        Injetado pelo Application para acesso ao adapter ativo.
        None significa sem transporte (modo dry-run / headless).

    stt_provider: STTProvider | None
        Provider de STT (Fase 4+). None = aceita FinalTranscript sintético (Fase 3 compat).
    """

    def __init__(
        self,
        bus: EventBus,
        config: Any = None,
        get_adapter: Callable[[], Any] | None = None,
        stt_provider: Any | None = None,
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

        # STT Provider Fase 4 (opcional — None = modo Fase 3)
        self._stt: Any | None = stt_provider

        # Métricas Fase 4
        self._metrics = MetricsRegistry(window=100)

        # Queue de eventos: o Orchestrator assina todos
        self._events = bus.subscribe(maxsize=-1)  # ilimitado para o maestro

    @property
    def adapter(self):
        """Acesso ao FirmwareAdapter ativo (ou None)."""
        return self._get_adapter()

    @property
    def metrics(self) -> MetricsRegistry:
        return self._metrics

    # -- Ciclo principal ----------------------------------------------------

    async def run(self) -> None:
        """Loop principal -- processa eventos do bus até ShutdownRequested."""
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
        if self._stt is not None:
            try:
                await self._stt.close()
            except Exception:
                pass
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
                pass  # publicados pelo RobotOutputProvider -- já no bus
            case SpeechDone():
                await self._on_speech_done(event)
            case _:
                log.debug("Orchestrator: evento não tratado %s", type(event).__name__)

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
            if self._stt is not None:
                self._stt.feed(event.pcm)

    async def _on_voice_end(self, event: VoiceActivityEnd) -> None:
        if not self._fsm.is_listening:
            return
        session = self._session
        if session is None:
            self._fsm.try_transition(TurnState.IDLE)
            return

        # Valida se há áudio suficiente
        if session.total_samples < 8000:  # < 500 ms
            session.discard_reason = "audio_curto"
            log.debug("Turno %d descartado: %s", session.turn_id, session.discard_reason)
            self._fsm.try_transition(TurnState.IDLE)
            await self._finish_turn()
            return

        session.mark("voice_end")
        t_voice_end = session.timeline["voice_end"]
        self._fsm.transition(TurnState.COMMITTING_TURN, turn_id=session.turn_id)

        if self._stt is not None:
            # Fase 4: STT real — lança worker assíncrono
            self._turn_task = asyncio.create_task(
                self._run_stt_worker(session, t_voice_end),
                name=f"nb_stt_{session.turn_id}",
            )
            log.info(
                "Turno %d comprometido: %d amostras (%.1f s) → STT iniciado",
                session.turn_id,
                session.total_samples,
                session.duration_s(),
            )
        else:
            # Fase 3 compat: aguarda FinalTranscript injetado externamente
            log.info(
                "Turno %d comprometido: %d amostras (%.1f s) — aguardando FinalTranscript (sem STT)",
                session.turn_id,
                session.total_samples,
                session.duration_s(),
            )

    async def _run_stt_worker(self, session: SessionContext, t_voice_end: float) -> None:
        """Task de STT: transcreve o áudio e publica FinalTranscript no bus."""
        turn_id = session.turn_id
        try:
            t_stt_start = time.monotonic()
            session.mark("stt_start", t_stt_start)

            audio_end_to_stt_start_ms = (t_stt_start - t_voice_end) * 1000.0

            full_pcm = session.full_pcm()
            ft = await self._stt.finalize(full_pcm, turn_id)

            t_stt_end = time.monotonic()
            session.mark("stt_end", t_stt_end)

            stt_ms = (t_stt_end - t_stt_start) * 1000.0
            end_of_turn_ms = (t_stt_end - session.t_start) * 1000.0

            # Registra métricas
            self._metrics.record("stt_ms", stt_ms)
            self._metrics.record("audio_end_to_stt_start_ms", audio_end_to_stt_start_ms)
            self._metrics.record("end_of_turn_ms", end_of_turn_ms)

            log.info(
                "STT turn_id=%d stt_ms=%.0f audio_end_to_stt_start=%.0f end_of_turn=%.0f",
                turn_id, stt_ms, audio_end_to_stt_start_ms, end_of_turn_ms,
            )

            await self._bus.publish(ft)

        except asyncio.CancelledError:
            log.info("STT worker turn_id=%d cancelado (barge-in?)", turn_id)
        except Exception:
            log.exception("STT worker turn_id=%d erro inesperado", turn_id)
            await self._bus.publish(TurnError(
                turn_id=turn_id,
                stage="stt",
                reason="unexpected_error",
            ))

    async def _on_final_transcript(self, event: FinalTranscript) -> None:
        """Fase 3/4: FinalTranscript (sintético ou de STT) → intent → robot.

        Pode chegar de:
          - STT real (Fase 4) — via _run_stt_worker
          - Injeção direta no bus (testes / debug CLI / Fase 3 compat)
        """
        # Garante que o turn_id bate (ou cria sessão sintética se chegou do bus direto)
        if self._session is None or self._session.turn_id != event.turn_id:
            if self._fsm.is_idle:
                # Injeção direta sem sessão de áudio -- aceita como turno sintético
                self._session = SessionContext(turn_id=event.turn_id)
                self._fsm.transition(TurnState.LISTENING, turn_id=event.turn_id)
                self._fsm.transition(TurnState.COMMITTING_TURN, turn_id=event.turn_id)
            else:
                log.debug(
                    "FinalTranscript turn_id=%d ignorado (sessão ativa=%s)",
                    event.turn_id,
                    self._session.turn_id if self._session else "None",
                )
                return

        session = self._session
        session.final_text = event.text
        session.mark("final_transcript")

        if not event.is_usable:
            log.debug("Turno %d: transcript não utilizável (%s)", event.turn_id, event.quality.name)
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
        t_intent_start = time.monotonic()
        intent = self._intent.match(
            text=event.text,
            turn_id=event.turn_id,
            context=context,
        )
        t_intent_end = time.monotonic()
        self._metrics.record("local_intent_ms", (t_intent_end - t_intent_start) * 1000.0)

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

            # Fase 3/4: sem TTS → SpeechDone imediato
            await self._bus.publish(SpeechDone(turn_id=event.turn_id))

        else:
            # Sem intent local → iria para LLM (Fase 5); por enquanto loga e encerra
            log.info(
                "Turno %d: sem intent local para %r -- iria à LLM (Fase 5)",
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
            # Métricas de latência de reação (primeira reação = expr enviada)
            t_start = session.t_start
            t_done = session.timeline.get("speech_done")
            if t_done:
                self._metrics.record("first_robot_reaction_ms", (t_done - t_start) * 1000.0)

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
        log.error("Erro no turno %d estágio=%s: %s", event.turn_id, event.stage, event.reason)
        await self._cancel_current_turn(reason=f"error:{event.stage}")
        self._fsm.try_transition(TurnState.ERROR_RECOVERY)
        self._fsm.try_transition(TurnState.IDLE)
        await self._finish_turn()

    # -- Helpers internos ---------------------------------------------------

    async def _begin_turn(self) -> None:
        """Inicia um novo turno: cria sessão e transiciona para LISTENING."""
        self._session = SessionContext(turn_id=new_turn_id())
        self._session.set_deadline(TURN_DEADLINE_S)
        self._fsm.transition(TurnState.LISTENING, turn_id=self._session.turn_id)
        if self._stt is not None:
            await self._stt.reset()
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
        """Limpa estado de sessão após o turno terminar."""
        if self._session:
            log.debug("Turno %d finalizado: %s", self._session.turn_id, self._session.to_log_dict())
            # Log de métricas ao final de cada turno
            snap = self._metrics.snapshot_flat()
            if snap:
                log.debug("Métricas (p50/p95): %s", {k: f"{v:.0f}ms" if v else "—" for k, v in snap.items()})
        self._session = None
        self._turn_task = None
