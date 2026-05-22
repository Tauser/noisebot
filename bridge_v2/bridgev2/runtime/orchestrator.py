"""bridgev2.runtime.orchestrator -- Maestro do event loop.

O Orchestrator é o único componente que cruza fronteiras de domínio.
Ele:
  - assina o bus para todos os eventos relevantes
  - delega ao TurnManager as transições de estado
  - chama os providers em sequência (STT → Intent → LLM → TTS → Robot)
  - gerencia a Task de turno (cancelada no barge-in — Invariante I-5)

Fase 1: esqueleto que sobe o loop, processa eventos de conexão, loga.
Fase 2: recebe get_adapter para enviar comandos ao firmware.
Fase 3: FinalTranscript → LocalIntentProvider → RobotOutputProvider → FSM completa.
Fase 4: STT real (faster-whisper) no COMMITTING_TURN via run_in_executor.
         Métricas: stt_ms, audio_end_to_stt_start_ms, end_of_turn_ms.
         STTProvider=None → continua aceitando FinalTranscript sintético (Fase 3 compat).
Fase 5: LLM streaming (OpenAI/Gemini) no THINKING quando não há intent local.
         Métricas: llm_first_token_ms, llm_total_ms.
         LLMProvider=None → encerra turno diretamente (sem resposta).
         Falha de API → TurnError → IDLE sem travar.
Fase 6: TTS persistente (PiperServerTTS) + cache + OutputScheduler.
         Ambos os caminhos (intent local e LLM) sintetizam e enviam SAY.
         Métricas: tts_first_audio_ms, first_audio_out_ms.
         TTSProvider=None → SpeechDone imediato (compat Fase 3–5).
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
    LlmTokenDelta,
    LlmReplyComplete,
    SentenceReady,
    SpeechDone,
    RobotCommand,
)
from .session import SessionContext, new_turn_id
from .turn_manager import TurnManager, TurnState
from ..llm.local_intent import LocalIntentProvider
from ..robot.output import RobotOutputProvider
from ..metrics.registry import MetricsRegistry
from ..tts.sentencizer import Sentencizer
from ..audio.playback import OutputScheduler
from ..audio.vad import BargeInMonitor

log = logging.getLogger(__name__)

TURN_DEADLINE_S = 30.0  # watchdog: turno máximo em segundos


class Orchestrator:
    """Coordena o pipeline de voz sobre o event loop asyncio.

    get_adapter: callable() -> FirmwareAdapter | None
        Injetado pelo Application para acesso ao adapter ativo.
        None significa sem transporte (modo dry-run / headless).

    stt_provider: STTProvider | None
        Provider de STT (Fase 4+). None = aceita FinalTranscript sintético.

    llm_provider: StreamingLLMProvider | LLMProvider | None
        Provider LLM (Fase 5+). None = encerra turno sem resposta LLM.
        Deve implementar generate_stream(text, context) → AsyncIterator[str].

    tts_provider: TTSProvider | None
        Provider TTS (Fase 6+). None = SpeechDone imediato (compat Fase 3–5).
        Deve implementar synthesize_stream(sentences) → AsyncIterator[bytes].

    status_store: StatusStore | None
        Estado de runtime compartilhado com a ops API (Fase 9.5).
    """

    def __init__(
        self,
        bus: EventBus,
        config: Any = None,
        get_adapter: Callable[[], Any] | None = None,
        stt_provider: Any | None = None,
        llm_provider: Any | None = None,
        tts_provider: Any | None = None,
        status_store: Any | None = None,
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

        # LLM Provider Fase 5 (opcional — None = sem resposta LLM)
        self._llm: Any | None = llm_provider

        # TTS Provider Fase 6 (opcional — None = SpeechDone imediato)
        self._tts: Any | None = tts_provider

        # VAD secundário Fase 7: detecta barge-in durante SPEAKING/THINKING
        self._vad = BargeInMonitor()
        self._t_barge_in: float | None = None   # timestamp para métrica

        # Watchdog Fase 10: task que cancela turnos preso além do deadline
        self._watchdog: asyncio.Task | None = None

        # StatusStore Fase 9.5: telemetria para a ops API
        self._store: Any | None = status_store

        # Métricas Fase 4+
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

    def set_llm_provider(self, provider: Any | None) -> None:
        """Atualiza o provider LLM em runtime (usado pelo ConfigController)."""
        self._llm = provider

    def set_tts_provider(self, provider: Any | None) -> None:
        """Atualiza o provider TTS em runtime.

        Usado pelo Application quando o Piper falha no boot e o bridge precisa
        seguir em modo sem voz sintetizada em vez de manter uma referência ruim.
        """
        self._tts = provider

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
        if self._tts is not None:
            try:
                await self._tts.shutdown()
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
            case LlmTokenDelta():
                pass  # consumido por TTS (Fase 6)
            case LlmReplyComplete():
                pass  # processado inline pelo _run_llm_worker
            case SentenceReady():
                pass  # consumido por TTS worker (Fase 6)
            case _:
                log.debug("Orchestrator: evento não tratado %s", type(event).__name__)

    # -- Handlers de estado -------------------------------------------------

    async def _on_firmware_connected(self, event: FirmwareConnected) -> None:
        log.info("Firmware conectado. capabilities=%s", event.peer_capabilities.get("features", []))
        self._fsm.reset_to_idle()
        if self._store:
            self._store.firmware_connected = True

    async def _on_firmware_disconnected(self, event: FirmwareDisconnected) -> None:
        log.warning("Firmware desconectado: %s", event.reason)
        await self._cancel_current_turn(reason="transport_disconnected")
        self._fsm.reset_to_idle()
        if self._store:
            self._store.firmware_connected = False

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

        # Fase 7: VAD secundário — detecta barge-in durante SPEAKING/THINKING
        if self._fsm.can_interrupt and self._session is not None:
            if self._vad.feed(event.pcm):
                log.info(
                    "VAD barge-in detectado no turno %d (energia sustentada)",
                    self._session.turn_id,
                )
                self._vad.reset()
                await self._bus.publish(BargeInDetected(turn_id=self._session.turn_id))

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
            await self._stt.initialize()
            ft = await self._stt.finalize(full_pcm, turn_id)

            t_stt_end = time.monotonic()
            session.mark("stt_end", t_stt_end)

            stt_ms = (t_stt_end - t_stt_start) * 1000.0
            end_of_turn_ms = (t_stt_end - session.t_start) * 1000.0

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
        """Fase 3/4/5: FinalTranscript → intent local → LLM (se sem intent) → robot.

        Pode chegar de:
          - STT real (Fase 4) — via _run_stt_worker
          - Injeção direta no bus (testes / debug CLI / Fase 3 compat)
        """
        # Garante que o turn_id bate (ou cria sessão sintética)
        if self._session is None or self._session.turn_id != event.turn_id:
            if self._fsm.is_idle:
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

        # Resolve intent local (< 5 ms, sem I/O)
        context: dict = {"status": {}}
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

        await self._bus.publish(intent)
        session.mark("intent_resolved")

        if intent.has_intent:
            # ── Caminho local intent ──────────────────────────────────────
            log.info(
                "Turno %d: intent=%s reply=%r",
                event.turn_id, intent.intent_name, intent.reply_text,
            )
            self._fsm.transition(TurnState.SPEAKING, turn_id=event.turn_id)
            session.mark("speaking_start")
            await self._robot.emit_for_intent(intent, self.adapter)

            if self._tts is not None and intent.reply_text:
                # Fase 6: sintetiza e envia SAY (cancelável por barge-in)
                self._turn_task = asyncio.create_task(
                    self._speak_reply(event.turn_id, intent.reply_text, session),
                    name=f"nb_tts_{event.turn_id}",
                )
            else:
                # Compat Fase 3–5: sem TTS → SpeechDone imediato
                await self._bus.publish(SpeechDone(turn_id=event.turn_id))

        elif self._llm is not None:
            # ── Caminho LLM (Fase 5) ──────────────────────────────────────
            log.info(
                "Turno %d: sem intent local para %r → LLM (%s)",
                event.turn_id, event.text, getattr(self._llm, "_provider_name", "?"),
            )
            self._turn_task = asyncio.create_task(
                self._run_llm_worker(session, event.text, event.turn_id),
                name=f"nb_llm_{event.turn_id}",
            )

        else:
            # ── Sem LLM configurado ───────────────────────────────────────
            log.info(
                "Turno %d: sem intent local para %r — sem LLM provider, encerrando turno",
                event.turn_id, event.text,
            )
            self._fsm.try_transition(TurnState.IDLE)
            await self._finish_turn()

    async def _run_llm_worker(
        self, session: SessionContext, text: str, turn_id: int
    ) -> None:
        """Task de LLM: stream de tokens → sentencizer → SentenceReady → robot.

        Métricas registradas: llm_first_token_ms, llm_total_ms.
        Em caso de erro: publica TurnError → _on_turn_error → IDLE.
        """
        t_llm_start = time.monotonic()
        first_token_recorded = False
        raw_tokens: list[str] = []

        try:
            context: dict = {
                "turn_id": turn_id,
                "robot_state": session.intent_name or "",
            }

            stream = self._llm.generate_stream(text, context)
            async for token in stream:
                if not first_token_recorded:
                    llm_first_ms = (time.monotonic() - t_llm_start) * 1000.0
                    self._metrics.record("llm_first_token_ms", llm_first_ms)
                    first_token_recorded = True

                raw_tokens.append(token)
                await self._bus.publish(LlmTokenDelta(turn_id=turn_id, text=token))

            t_llm_end = time.monotonic()
            llm_total_ms = (t_llm_end - t_llm_start) * 1000.0
            self._metrics.record("llm_total_ms", llm_total_ms)

            raw_response = "".join(raw_tokens)
            session.mark("llm_complete")

            # ── Parseia JSON de resposta ───────────────────────────────────
            from ..llm.prompt import parse_llm_json
            try:
                parsed = parse_llm_json(raw_response)
            except (ValueError, Exception) as exc:
                log.warning(
                    "Turno %d: falha ao parsear JSON LLM (%s). Usando raw como reply.",
                    turn_id, exc,
                )
                parsed = {
                    "reply": raw_response.strip(),
                    "expression_id": None,
                    "action": None,
                    "emot_event": None,
                }

            reply_text = parsed["reply"]

            # ── Sentencizer → SentenceReady ────────────────────────────────
            sz = Sentencizer()
            sentences: list[str] = list(sz.feed(reply_text)) + list(sz.flush())

            for idx, sentence in enumerate(sentences):
                await self._bus.publish(
                    SentenceReady(turn_id=turn_id, sentence=sentence, index=idx)
                )

            # ── LlmReplyComplete ───────────────────────────────────────────
            complete = LlmReplyComplete(
                turn_id=turn_id,
                reply=reply_text,
                expression_id=parsed.get("expression_id"),
                action_id=parsed.get("action"),
                emot_event_id=parsed.get("emot_event"),
                provider=getattr(self._llm, "_provider_name", "unknown"),
                model=getattr(self._llm, "_model", ""),
            )
            await self._bus.publish(complete)

            log.info(
                "LLM turn_id=%d first_token=%.0fms total=%.0fms reply=%r",
                turn_id, llm_first_ms if first_token_recorded else 0,
                llm_total_ms, reply_text[:80],
            )

            # ── Robot output via IntentResolved sintético ──────────────────
            llm_intent = IntentResolved(
                turn_id=turn_id,
                intent_name="llm_reply",
                reply_text=reply_text,
                expression_id=complete.expression_id,
                action_id=complete.action_id,
                emot_event_id=complete.emot_event_id,
            )
            session.intent_name = "llm_reply"
            session.reply_text = reply_text

            # THINKING → SPEAKING + reação visual imediata (< 300 ms)
            self._fsm.transition(TurnState.SPEAKING, turn_id=turn_id)
            session.mark("speaking_start")
            await self._robot.emit_for_intent(llm_intent, self.adapter)

            # Fase 6: TTS síntese frase a frase + SAY paginado
            if self._tts is not None and sentences:
                await self._run_tts_and_speak(turn_id, sentences, session)
            await self._bus.publish(SpeechDone(turn_id=turn_id))

        except asyncio.CancelledError:
            log.info("LLM worker turn_id=%d cancelado (barge-in?)", turn_id)
        except Exception as exc:
            log.exception("LLM worker turn_id=%d erro: %s", turn_id, exc)
            await self._bus.publish(TurnError(
                turn_id=turn_id,
                stage="llm",
                reason=type(exc).__name__,
            ))

    async def _speak_reply(
        self, turn_id: int, reply_text: str, session: SessionContext
    ) -> None:
        """Task de TTS para o caminho de intent local. Cancelável por barge-in."""
        try:
            sz = Sentencizer()
            sentences = list(sz.feed(reply_text)) + list(sz.flush())
            if sentences:
                await self._run_tts_and_speak(turn_id, sentences, session)
            await self._bus.publish(SpeechDone(turn_id=turn_id))
        except asyncio.CancelledError:
            log.info("TTS reply turn_id=%d cancelado (barge-in?)", turn_id)
        except Exception as exc:
            log.exception("TTS reply turn_id=%d erro: %s", turn_id, exc)
            await self._bus.publish(
                TurnError(turn_id=turn_id, stage="tts", reason=type(exc).__name__)
            )

    async def _run_tts_and_speak(
        self,
        turn_id: int,
        sentences: list[str],
        session: SessionContext,
    ) -> None:
        """Sintetiza sentences via TTS e envia SAY ao firmware com pacing.

        Registra tts_first_audio_ms e first_audio_out_ms ao enviar o 1º chunk.
        """
        first_audio_recorded = False

        def _on_first(tid: int) -> None:
            nonlocal first_audio_recorded
            if not first_audio_recorded:
                first_audio_recorded = True
                t = time.monotonic()
                elapsed_ms = (t - session.t_start) * 1000.0
                self._metrics.record("tts_first_audio_ms", elapsed_ms)
                self._metrics.record("first_audio_out_ms", elapsed_ms)
                session.mark("first_audio_out", t)
                log.info("Turno %d: primeiro SAY %.0f ms de VOICE_END", tid, elapsed_ms)

        scheduler = OutputScheduler()

        async def _aiter_sentences():
            for s in sentences:
                yield s

        await scheduler.run(
            turn_id=turn_id,
            pcm_iter=self._tts.synthesize_stream(_aiter_sentences()),
            adapter=self.adapter,
            on_first_audio=_on_first,
        )

    async def _on_speech_done(self, event: SpeechDone) -> None:
        """Turno de fala encerrado → volta a IDLE com baseline."""
        if self._session and self._session.turn_id != event.turn_id:
            return
        session = self._session
        if session:
            session.mark("speech_done")
            t_start = session.t_start
            t_done = session.timeline.get("speech_done")
            if t_done:
                self._metrics.record("first_robot_reaction_ms", (t_done - t_start) * 1000.0)

        await self._robot.reset_baseline(self.adapter, event.turn_id)
        self._fsm.try_transition(TurnState.IDLE)
        # Telemetria: registra outcome do turno
        if self._store and session:
            intent = session.intent_name or ""
            outcome = "llm" if intent == "llm_reply" else ("local_intent" if intent else "ok")
            self._store.record_turn(event.turn_id, outcome)
        await self._finish_turn()

    async def _on_barge_in(self, event: BargeInDetected) -> None:
        if not self._fsm.can_interrupt:
            return
        if self._session is None or self._session.turn_id != event.turn_id:
            log.debug(
                "Barge-in stale ignorado: event_turn=%d session_turn=%s",
                event.turn_id,
                self._session.turn_id if self._session else "None",
            )
            return

        t_barge = time.monotonic()
        log.info("Barge-in no turno %d (estado=%s)", event.turn_id, self._fsm.state.name)

        adapter = self.adapter

        # Barge-in cristalino: SPEECH_CANCEL se firmware suportar.
        # Barge-in suave: parar de enviar SAY (OutputScheduler cancelado junto com a Task).
        if adapter is not None:
            await adapter.send_speech_cancel(event.turn_id)

        # Cancela Task de turno (LLM stream + TTS + OutputScheduler)
        await self._cancel_current_turn(reason="barge_in")

        t_cancelled = time.monotonic()
        self._metrics.record("interruption_cancel_ms", (t_cancelled - t_barge) * 1000.0)
        log.info("Barge-in: turno %d cancelado em %.0f ms", event.turn_id, (t_cancelled - t_barge) * 1000.0)
        if self._store:
            self._store.record_turn(event.turn_id, "interrupted")

        # Restaura baseline IDLE no robô (regra de baseline do CLAUDE.md)
        await self._robot.reset_baseline(adapter, event.turn_id)

        # VAD reset — limpa contadores para o próximo turno
        self._vad.reset()

        # INTERRUPTED → LISTENING: inicia novo turno para capturar a nova fala
        self._fsm.try_transition(TurnState.INTERRUPTED)

        # Nova sessão (novo turn_id monotônico)
        self._session = SessionContext(turn_id=new_turn_id())
        self._session.set_deadline(TURN_DEADLINE_S)
        self._fsm.try_transition(TurnState.LISTENING, turn_id=self._session.turn_id)
        if self._stt is not None:
            await self._stt.reset()

    async def _on_turn_error(self, event: TurnError) -> None:
        log.error("Erro no turno %d estágio=%s: %s", event.turn_id, event.stage, event.reason)
        if self._store:
            self._store.record_turn(event.turn_id, "failed")
            self._store.record_error(
                kind=f"{event.stage}_failure",
                turn_id=event.turn_id,
                message=event.reason[:200],
            )
        await self._cancel_current_turn(reason=f"error:{event.stage}")
        self._fsm.try_transition(TurnState.ERROR_RECOVERY)
        self._fsm.try_transition(TurnState.IDLE)
        await self._finish_turn()

    # -- Helpers internos ---------------------------------------------------

    async def _begin_turn(self) -> None:
        """Inicia um novo turno: cria sessão, transiciona para LISTENING e arma watchdog."""
        self._session = SessionContext(turn_id=new_turn_id())
        self._session.set_deadline(TURN_DEADLINE_S)
        self._fsm.transition(TurnState.LISTENING, turn_id=self._session.turn_id)
        if self._stt is not None:
            await self._stt.reset()
        # Fase 10: watchdog garante que o turno sempre termina (Invariante I-4)
        if self._watchdog and not self._watchdog.done():
            self._watchdog.cancel()
        self._watchdog = asyncio.create_task(
            self._run_watchdog(self._session),
            name=f"nb_watchdog_{self._session.turn_id}",
        )
        log.info("Turno %d iniciado (LISTENING)", self._session.turn_id)

    async def _run_watchdog(self, session: SessionContext) -> None:
        """Cancela o turno se o deadline for excedido — Invariante I-4."""
        try:
            while True:
                await asyncio.sleep(2.0)
                if self._session is not session:
                    return   # turno terminou normalmente
                if session.is_past_deadline():
                    log.warning(
                        "Watchdog: turno %d excedeu deadline de %.0f s — cancelando",
                        session.turn_id, TURN_DEADLINE_S,
                    )
                    if self._store:
                        self._store.record_error(
                            kind="watchdog_timeout",
                            turn_id=session.turn_id,
                            message=f"deadline {TURN_DEADLINE_S}s excedido",
                        )
                    await self._bus.publish(TurnError(
                        turn_id=session.turn_id,
                        stage="watchdog",
                        reason="deadline_exceeded",
                    ))
                    return
        except asyncio.CancelledError:
            pass

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
            snap = self._metrics.snapshot_flat()
            if snap:
                log.debug("Métricas (p50/p95): %s", {k: f"{v:.0f}ms" if v else "—" for k, v in snap.items()})
        self._session = None
        self._turn_task = None
        # Cancela watchdog — turno terminou dentro do prazo
        if self._watchdog and not self._watchdog.done():
            self._watchdog.cancel()
        self._watchdog = None
