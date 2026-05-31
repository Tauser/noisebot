"""NoiseBot server runtime orchestrator.

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
from collections import deque
from typing import Any, Callable

from .intents import LocalIntentProvider
from .metrics import MetricsRegistry
from .output import RobotOutputProvider
from .playback import OutputScheduler
from .runtime import (
    AudioChunkIn,
    BargeInDetected,
    FirmwareConnected,
    FirmwareDisconnected,
    FinalTranscript,
    IntentResolved,
    LlmReplyComplete,
    LlmTokenDelta,
    RobotCommand,
    SentenceReady,
    SessionContext,
    ShutdownRequested,
    SpeechDone,
    StatusUpdate,
    TranscriptQuality,
    TurnError,
    TurnManager,
    TurnState,
    VoiceActivityEnd,
    VoiceActivityStart,
    WakeDetected,
    new_turn_id,
    EventBus,
)
from .tts import Sentencizer
from .vad import BargeInMonitor
from ..vision import VisionClient

log = logging.getLogger(__name__)

DEFAULT_MAX_UTTERANCE_SAMPLES = 192000
TURN_DEADLINE_S = 30.0  # watchdog antes de iniciar fala
PLAYBACK_BARGE_IN_GRACE_S = 0.9
ENABLE_SECONDARY_BARGE_IN_VAD = False
SPEAKING_PROGRESS_DEADLINE_S = 20.0  # durante SAY: tempo maximo sem progresso
EMPTY_WAKE_REPLY = "Oi! Em que posso ajudar?"
MISUNDERSTOOD_REPLY = "Eu ouvi, mas não entendi direito. Pode repetir?"
MISUNDERSTOOD_PROMPT_COOLDOWN_S = 45.0
MISUNDERSTOOD_MAX_NO_SPEECH_PROB = 0.50
LLM_CONFIG_FALLBACK_REPLY = (
    "Estou sem acesso à IA agora, mas continuo ouvindo os comandos locais."
)
TTS_FAST_FRAGMENT_MAX_CHARS = 96
TTS_FAST_FRAGMENT_MIN_CHARS = 24
TEXT_SCROLL_MAX_BYTES = 128
TEXT_SCROLL_PAGE_INTERVAL_S = 2.2


def _sentences_for_tts(text: str) -> list[str]:
    """Prepara frases curtas o bastante para o Piper iniciar o áudio cedo."""
    sz = Sentencizer()
    sentences = list(sz.feed(text)) + list(sz.flush())
    prepared: list[str] = []
    for sentence in sentences:
        prepared.extend(_split_tts_sentence(sentence))
    return prepared


def _split_tts_sentence(sentence: str) -> list[str]:
    clean = " ".join(sentence.split())
    if len(clean) <= TTS_FAST_FRAGMENT_MAX_CHARS:
        return [clean] if clean else []

    parts: list[str] = []
    current = ""
    for token in clean.replace("; ", ";|").replace(", ", ",|").split("|"):
        token = token.strip()
        if not token:
            continue
        candidate = f"{current} {token}".strip() if current else token
        if current and len(candidate) > TTS_FAST_FRAGMENT_MAX_CHARS:
            if len(current) >= TTS_FAST_FRAGMENT_MIN_CHARS:
                parts.append(current)
                current = token
            else:
                current = candidate
        else:
            current = candidate
    if current:
        parts.append(current)
    return parts or ([clean] if clean else [])


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
        self._intent = LocalIntentProvider(
            vision_client=VisionClient.from_config(config) if config is not None else None
        )
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
        self._last_status: dict[str, Any] = {}

        # Métricas Fase 4+
        self._metrics = MetricsRegistry(window=100)
        self._empty_wake_prompted_at: float | None = None
        self._misunderstood_prompted_at: float | None = None
        self._misunderstood_prompt_pending = False
        self._recent_llm_replies: deque[str] = deque(maxlen=6)

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
        await self._finish_turn()
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
                self._last_status = {
                    "state": event.state,
                    "valence": event.valence,
                    "activation": event.activation,
                    "attention": event.attention,
                    "health": event.health,
                }
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
        await self._finish_turn()
        if self._store:
            self._store.firmware_connected = False

    async def _on_wake(self, event: WakeDetected) -> None:
        if self._fsm.is_idle:
            await self._begin_turn()
            return
        if self._fsm.can_interrupt:
            await self._bus.publish(BargeInDetected(turn_id=self._fsm.current_turn_id))
            return
        if not self._fsm.is_idle:
            log.debug("Wake detectado fora de IDLE (estado=%s) -- ignorado", self._fsm.state.name)
            return

    async def _on_voice_start(self, event: VoiceActivityStart) -> None:
        if self._fsm.is_idle:
            await self._begin_turn()
        elif self._fsm.can_interrupt:
            await self._bus.publish(BargeInDetected(turn_id=self._fsm.current_turn_id))

    async def _on_audio_chunk(self, event: AudioChunkIn) -> None:
        if self._session and self._fsm.is_listening:
            self._session.append_audio(event.pcm)
            max_samples = self._max_utterance_samples()
            if max_samples > 0 and self._session.total_samples > max_samples:
                log.warning(
                    "Turno %d descartado: audio_longo samples=%d limite=%d",
                    self._session.turn_id,
                    self._session.total_samples,
                    max_samples,
                )
                self._session.discard_reason = "audio_longo"
                self._session.meta["outcome"] = "audio_rejected"
                self._session.meta["error_reason"] = "max_utterance_samples"
                self._fsm.try_transition(TurnState.IDLE)
                await self._finish_turn()
                return
            if self._stt is not None:
                self._stt.feed(event.pcm)

        # Fase 7: VAD secundário — detecta barge-in durante SPEAKING/THINKING
        if ENABLE_SECONDARY_BARGE_IN_VAD and self._fsm.can_interrupt and self._session is not None:
            first_audio = self._session.timeline.get("first_audio_out")
            playback_age = (
                time.monotonic() - first_audio
                if first_audio is not None
                else None
            )
            allow_barge = (
                self._fsm.state == TurnState.THINKING
                or playback_age is None
                or playback_age >= PLAYBACK_BARGE_IN_GRACE_S
            )
            if self._vad.feed(event.pcm, allow_trigger=allow_barge):
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
            session.meta["outcome"] = "audio_rejected"
            session.meta["voice_end_reason"] = event.reason.name.lower()
            log.debug("Turno %d descartado: %s", session.turn_id, session.discard_reason)
            self._fsm.try_transition(TurnState.IDLE)
            await self._finish_turn()
            return

        session.mark("voice_end")
        session.meta["voice_end_reason"] = event.reason.name.lower()
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
        session.meta["transcript_quality"] = event.quality.name.lower()
        session.meta["no_speech_prob"] = round(event.no_speech_prob, 3)
        session.meta["avg_logprob"] = round(event.avg_logprob, 3)
        session.meta["compression_ratio"] = round(event.compression_ratio, 3)
        if self._store:
            self._store.record_turn_detail(event.turn_id, transcript=event.text)

        if not event.is_usable:
            if self._should_prompt_after_empty_wake(session, event):
                await self._prompt_after_empty_wake(session, event)
                return
            if self._should_prompt_after_unclear_transcript(session, event):
                await self._prompt_after_unclear_transcript(session, event)
                return
            log.debug("Turno %d: transcript não utilizável (%s)", event.turn_id, event.quality.name)
            session.discard_reason = f"stt_{event.quality.name.lower()}"
            session.meta["outcome"] = "stt_rejected"
            self._fsm.try_transition(TurnState.IDLE)
            await self._finish_turn()
            return

        self._empty_wake_prompted_at = None
        self._misunderstood_prompted_at = None
        self._misunderstood_prompt_pending = False

        # COMMITTING_TURN → THINKING
        if self._fsm.state != TurnState.COMMITTING_TURN:
            self._fsm.try_transition(TurnState.COMMITTING_TURN, turn_id=event.turn_id)
        self._fsm.transition(TurnState.THINKING, turn_id=event.turn_id)
        session.mark("thinking_start")

        # Resolve intent local (< 5 ms, sem I/O)
        context: dict = {"status": self._last_status}
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
            if self._store:
                self._store.record_turn_detail(
                    event.turn_id,
                    reply=intent.reply_text,
                    route="local_intent" if intent.has_intent else "",
                )

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
            has_tts_reply = self._tts is not None and bool(intent.reply_text)
            await self._robot.emit_for_intent(
                intent,
                self.adapter,
                include_reply_text=not has_tts_reply,
            )

            if self._tts is not None and intent.reply_text:
                # Fase 6: sintetiza e envia SAY (cancelável por barge-in)
                log.info(
                    "Turno %d: iniciando TTS local para intent=%s",
                    event.turn_id,
                    intent.intent_name,
                )
                self._turn_task = asyncio.create_task(
                    self._speak_reply(event.turn_id, intent.reply_text, session),
                    name=f"nb_tts_{event.turn_id}",
                )
            else:
                # Compat Fase 3–5: sem TTS → SpeechDone imediato
                if intent.reply_text:
                    log.warning(
                        "Turno %d: resposta tem texto, mas TTS esta desabilitado",
                        event.turn_id,
                    )
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

    def _should_prompt_after_empty_wake(
        self,
        session: SessionContext,
        event: FinalTranscript,
    ) -> bool:
        """True quando houve wake/voz real, mas o usuario nao falou nada util."""
        if event.quality not in (TranscriptQuality.EMPTY, TranscriptQuality.NO_SPEECH):
            return False
        if self._empty_wake_prompted_at is not None:
            log.info(
                "Turno %d: wake vazio ignorado; prompt ja enviado nesta sequencia",
                event.turn_id,
            )
            return False
        return session.total_samples >= 8000

    async def _prompt_after_empty_wake(
        self,
        session: SessionContext,
        event: FinalTranscript,
    ) -> None:
        """Responde localmente quando o usuario chama o robo e fica em silencio."""
        log.info(
            "Turno %d: wake sem fala utilizavel (%s) -> prompt local",
            event.turn_id,
            event.quality.name,
        )
        self._empty_wake_prompted_at = time.monotonic()

        if self._fsm.state != TurnState.COMMITTING_TURN:
            self._fsm.try_transition(TurnState.COMMITTING_TURN, turn_id=event.turn_id)
        self._fsm.transition(TurnState.THINKING, turn_id=event.turn_id)
        session.mark("thinking_start")

        intent = IntentResolved(
            turn_id=event.turn_id,
            intent_name="local_empty_wake_prompt",
            reply_text=EMPTY_WAKE_REPLY,
            expression_id=3,  # HAPPY
            action_id=1,      # nod
            emot_event_id=3,  # happy
        )
        session.intent_name = intent.intent_name
        session.reply_text = EMPTY_WAKE_REPLY
        if self._store:
            self._store.record_turn_detail(
                event.turn_id,
                transcript=event.text,
                reply=EMPTY_WAKE_REPLY,
                route="local_intent",
            )

        await self._bus.publish(intent)
        session.mark("intent_resolved")

        self._fsm.transition(TurnState.SPEAKING, turn_id=event.turn_id)
        session.mark("speaking_start")
        await self._robot.emit_for_intent(
            intent,
            self.adapter,
            include_reply_text=self._tts is None,
        )

        if self._tts is not None:
            self._turn_task = asyncio.create_task(
                self._speak_reply(event.turn_id, EMPTY_WAKE_REPLY, session),
                name=f"nb_tts_{event.turn_id}",
            )
        else:
            await self._bus.publish(SpeechDone(turn_id=event.turn_id))

    def _should_prompt_after_unclear_transcript(
        self,
        session: SessionContext,
        event: FinalTranscript,
    ) -> bool:
        """True quando houve fala, mas a transcrição ficou insegura."""
        if event.quality not in (
            TranscriptQuality.LOW_RMS,
            TranscriptQuality.LOW_LOGPROB,
            TranscriptQuality.HIGH_COMPRESSION,
        ):
            return False
        if session.total_samples < 8000:
            return False
        if event.no_speech_prob > MISUNDERSTOOD_MAX_NO_SPEECH_PROB:
            log.info(
                "Turno %d: transcript incerto tratado como ruido/silencio (%s no_speech=%.2f)",
                event.turn_id,
                event.quality.name,
                event.no_speech_prob,
            )
            return False
        if self._misunderstood_prompt_pending:
            log.info(
                "Turno %d: prompt de nao entendimento ja enviado; aguardando fala utilizavel",
                event.turn_id,
            )
            return False
        now = time.monotonic()
        if (
            self._misunderstood_prompted_at is not None
            and now - self._misunderstood_prompted_at < MISUNDERSTOOD_PROMPT_COOLDOWN_S
        ):
            log.info(
                "Turno %d: prompt de nao entendimento em cooldown (%s)",
                event.turn_id,
                event.quality.name,
            )
            return False
        return True

    async def _prompt_after_unclear_transcript(
        self,
        session: SessionContext,
        event: FinalTranscript,
    ) -> None:
        """Pede repetição quando o usuário falou, mas o STT ficou incerto."""
        reply = MISUNDERSTOOD_REPLY
        log.info(
            "Turno %d: transcript incerto (%s) -> pedido de repeticao",
            event.turn_id,
            event.quality.name,
        )
        self._misunderstood_prompted_at = time.monotonic()
        self._misunderstood_prompt_pending = True

        if self._fsm.state != TurnState.COMMITTING_TURN:
            self._fsm.try_transition(TurnState.COMMITTING_TURN, turn_id=event.turn_id)
        self._fsm.transition(TurnState.THINKING, turn_id=event.turn_id)
        session.mark("thinking_start")

        intent = IntentResolved(
            turn_id=event.turn_id,
            intent_name="local_unclear_transcript_prompt",
            reply_text=reply,
            expression_id=4,  # FOCUSED
            action_id=1,      # nod
            emot_event_id=1,
        )
        session.intent_name = intent.intent_name
        session.reply_text = reply
        session.meta["unclear_reason"] = event.quality.name.lower()
        if self._store:
            self._store.record_turn_detail(
                event.turn_id,
                transcript=event.text,
                reply=reply,
                route="local_intent",
            )

        await self._bus.publish(intent)
        session.mark("intent_resolved")

        self._fsm.transition(TurnState.SPEAKING, turn_id=event.turn_id)
        session.mark("speaking_start")
        await self._robot.emit_for_intent(
            intent,
            self.adapter,
            include_reply_text=self._tts is None,
        )

        if self._tts is not None:
            self._turn_task = asyncio.create_task(
                self._speak_reply(event.turn_id, reply, session),
                name=f"nb_tts_{event.turn_id}",
            )
        else:
            await self._bus.publish(SpeechDone(turn_id=event.turn_id))

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
                "recent_replies": list(self._recent_llm_replies),
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
            from .llm import enforce_pt_br_reply, parse_llm_json, recover_llm_reply_text
            try:
                parsed = parse_llm_json(raw_response)
            except (ValueError, Exception) as exc:
                log.warning(
                    "Turno %d: falha ao parsear JSON LLM (%s). Usando raw como reply.",
                    turn_id, exc,
                )
                parsed = {
                    "reply": recover_llm_reply_text(raw_response),
                    "expression_id": None,
                    "action": None,
                    "emot_event": None,
                }

            reply_text, language_replaced = enforce_pt_br_reply(parsed["reply"], text)
            if language_replaced:
                log.warning(
                    "Turno %d: reply LLM substituida por guard de idioma pt-BR",
                    turn_id,
                )
            if reply_text:
                self._recent_llm_replies.append(reply_text)

            # ── Sentencizer → SentenceReady ────────────────────────────────
            sentences = _sentences_for_tts(reply_text)

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
            if self._store:
                self._store.record_turn_detail(
                    turn_id,
                    transcript=session.final_text,
                    reply=reply_text,
                    route="llm",
                )

            # THINKING → SPEAKING + reação visual imediata (< 300 ms)
            self._fsm.transition(TurnState.SPEAKING, turn_id=turn_id)
            session.mark("speaking_start")
            has_tts_reply = self._tts is not None and bool(sentences)
            await self._robot.emit_for_intent(
                llm_intent,
                self.adapter,
                include_reply_text=not has_tts_reply,
            )

            # Fase 6: TTS síntese frase a frase + SAY paginado
            if self._tts is not None and sentences:
                await self._run_tts_and_speak(turn_id, sentences, session)
            await self._bus.publish(SpeechDone(turn_id=turn_id))

        except asyncio.CancelledError:
            log.info("LLM worker turn_id=%d cancelado (barge-in?)", turn_id)
        except Exception as exc:
            if self._is_llm_config_error(exc):
                log.warning(
                    "LLM indisponível por configuração turn_id=%d: %s",
                    turn_id,
                    exc,
                )
                try:
                    await self._emit_llm_fallback(
                        session=session,
                        turn_id=turn_id,
                        transcript=text,
                        reply_text=LLM_CONFIG_FALLBACK_REPLY,
                        reason="llm_config",
                    )
                    return
                except Exception as fallback_exc:
                    log.exception(
                        "Fallback LLM turn_id=%d falhou: %s",
                        turn_id,
                        fallback_exc,
                    )
            log.exception("LLM worker turn_id=%d erro: %s", turn_id, exc)
            await self._bus.publish(TurnError(
                turn_id=turn_id,
                stage="llm",
                reason=type(exc).__name__,
            ))

    @staticmethod
    def _is_llm_config_error(exc: Exception) -> bool:
        return (
            isinstance(exc, ValueError)
            and "OPENAI_API_KEY" in str(exc)
        )

    async def _emit_llm_fallback(
        self,
        session: SessionContext,
        turn_id: int,
        transcript: str,
        reply_text: str,
        reason: str,
    ) -> None:
        sentences = _sentences_for_tts(reply_text)

        for idx, sentence in enumerate(sentences):
            await self._bus.publish(
                SentenceReady(turn_id=turn_id, sentence=sentence, index=idx)
            )

        complete = LlmReplyComplete(
            turn_id=turn_id,
            reply=reply_text,
            expression_id=4,
            action_id=0,
            emot_event_id=0,
            provider="local_fallback",
            model=reason,
        )
        await self._bus.publish(complete)

        fallback_intent = IntentResolved(
            turn_id=turn_id,
            intent_name="llm_fallback",
            reply_text=reply_text,
            expression_id=complete.expression_id,
            action_id=complete.action_id,
            emot_event_id=complete.emot_event_id,
        )
        session.intent_name = "llm_fallback"
        session.reply_text = reply_text
        if self._store:
            self._store.record_turn_detail(
                turn_id,
                transcript=transcript,
                reply=reply_text,
                route="fallback",
            )

        self._fsm.transition(TurnState.SPEAKING, turn_id=turn_id)
        session.mark("speaking_start")
        has_tts_reply = self._tts is not None and bool(sentences)
        await self._robot.emit_for_intent(
            fallback_intent,
            self.adapter,
            include_reply_text=not has_tts_reply,
        )

        if self._tts is not None and sentences:
            await self._run_tts_and_speak(turn_id, sentences, session)
        await self._bus.publish(SpeechDone(turn_id=turn_id))

    async def _speak_reply(
        self, turn_id: int, reply_text: str, session: SessionContext
    ) -> None:
        """Task de TTS para o caminho de intent local. Cancelável por barge-in."""
        try:
            sentences = _sentences_for_tts(reply_text)
            log.info(
                "TTS reply turn_id=%d preparado frases=%d chars=%d",
                turn_id,
                len(sentences),
                len(reply_text),
            )
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

        Registra a latência real do TTS e o tempo de resposta do turno.
        """
        first_audio_recorded = False
        session.mark("tts_start")
        session.meta["tts_sentence_count"] = len(sentences)
        session.meta["tts_text_chars"] = sum(len(sentence) for sentence in sentences)

        async def _on_first(tid: int) -> None:
            nonlocal first_audio_recorded
            if not first_audio_recorded:
                first_audio_recorded = True
                t = time.monotonic()
                turn_elapsed_ms = (t - session.t_start) * 1000.0
                tts_start = session.timeline.get("tts_start", t)
                tts_first_ms = (t - tts_start) * 1000.0
                voice_end = session.timeline.get("voice_end")
                voice_end_elapsed_ms = (
                    (t - voice_end) * 1000.0
                    if voice_end is not None
                    else turn_elapsed_ms
                )
                self._metrics.record("tts_first_audio_ms", tts_first_ms)
                self._metrics.record("first_audio_out_ms", turn_elapsed_ms)
                session.mark("first_audio_out", t)
                self._vad.reset()
                log.info(
                    "Turno %d: primeiro SAY tts=%.0fms pos_fim_voz=%.0fms turno=%.0fms",
                    tid,
                    tts_first_ms,
                    voice_end_elapsed_ms,
                    turn_elapsed_ms,
                )
                if self.adapter is not None and session.reply_text:
                    asyncio.create_task(
                        self._send_reply_text_scroll(session),
                        name=f"nb_reply_text_{tid}",
                    )

        def _on_progress(_tid: int) -> None:
            session.set_deadline(SPEAKING_PROGRESS_DEADLINE_S)

        session.set_deadline(SPEAKING_PROGRESS_DEADLINE_S)
        scheduler = OutputScheduler()

        async def _aiter_sentences():
            for s in sentences:
                yield s

        stats = await scheduler.run(
            turn_id=turn_id,
            pcm_iter=self._tts.synthesize_stream(_aiter_sentences()),
            adapter=self.adapter,
            on_first_audio=_on_first,
            on_audio_progress=_on_progress,
        )
        session.meta["tts_chunks_sent"] = stats.chunks_sent
        session.meta["tts_pcm_bytes_in"] = stats.pcm_bytes_in
        session.meta["tts_pcm_bytes_sent"] = stats.pcm_bytes_sent
        session.meta["tts_padding_bytes"] = stats.padding_bytes
        session.meta["tts_say_begin_sent"] = stats.say_begin_sent
        session.meta["tts_say_end_sent"] = stats.say_end_sent
        session.meta["tts_expected_duration_ms"] = round(
            stats.chunks_sent * 16.0,
            1,
        )
        session.meta["tts_completed"] = bool(stats.chunks_sent and stats.say_end_sent)
        if not first_audio_recorded:
            log.warning(
                "Turno %d: TTS nao gerou nenhum chunk de audio para %d frase(s)",
                turn_id,
                len(sentences),
            )

    async def _send_reply_text_scroll(self, session: SessionContext) -> None:
        adapter = self.adapter
        text = session.reply_text or ""
        if adapter is None or not text:
            return
        encoded = text.encode("utf-8")
        pages = _split_text_scroll_pages(text)
        session.meta["text_scroll_chars"] = len(text)
        session.meta["text_scroll_bytes"] = len(encoded)
        session.meta["text_scroll_payload_bytes"] = min(len(encoded), TEXT_SCROLL_MAX_BYTES)
        session.meta["text_scroll_truncated"] = len(encoded) > TEXT_SCROLL_MAX_BYTES
        session.meta["text_scroll_pages"] = len(pages)
        session.meta["text_scroll_pages_sent"] = 0
        try:
            for index, page in enumerate(pages):
                if index > 0:
                    await asyncio.sleep(TEXT_SCROLL_PAGE_INTERVAL_S)
                await adapter.send_text_scroll(page)
                session.meta["text_scroll_pages_sent"] = index + 1
        except Exception as exc:
            log.warning("Text scroll de resposta falhou sem bloquear TTS: %s", exc)

    async def _arm_followup_if_needed(self, session: SessionContext) -> None:
        adapter = self.adapter
        if adapter is None:
            return
        if not self._should_arm_followup(session):
            return
        payload = {
            "event": "FOLLOWUP_ARM",
            "turn_id": session.turn_id,
            "window_ms": 8000,
            "source": session.intent_name or "reply",
        }
        if await self._send_session_event(payload, "FOLLOWUP_ARM"):
            session.meta["followup_armed"] = True

    def _should_arm_followup(self, session: SessionContext) -> bool:
        if session.meta.get("outcome") not in ("local_intent", "llm"):
            return False
        if session.intent_name == "local_empty_wake_prompt":
            return False
        reply = (session.reply_text or "").strip()
        return reply.endswith("?") or "?" in reply[-80:]

    async def _send_session_event(self, payload: dict[str, Any], label: str) -> bool:
        adapter = self.adapter
        if adapter is None:
            return False
        send_session = getattr(adapter, "send_session", None)
        if send_session is None:
            return False
        try:
            await send_session(payload)
            return True
        except Exception as exc:
            log.warning("%s falhou sem bloquear turno: %s", label, exc)
            return False

    async def _send_terminal_session_event(self, session: SessionContext) -> None:
        if session.meta.get("followup_armed"):
            return
        outcome = str(session.meta.get("outcome") or "")
        if outcome == "failed":
            payload = {
                "event": "SESSION_ERROR",
                "turn_id": session.turn_id,
                "stage": session.meta.get("error_stage") or "",
                "reason": session.meta.get("error_reason") or session.discard_reason or "",
            }
            await self._send_session_event(payload, "SESSION_ERROR")
        elif outcome in ("cancelled", "interrupted") or session.discard_reason:
            payload = {
                "event": "FOLLOWUP_CANCEL",
                "turn_id": session.turn_id,
                "reason": session.discard_reason or outcome or "cancelled",
            }
            await self._send_session_event(payload, "FOLLOWUP_CANCEL")
        else:
            payload = {
                "event": "SESSION_DONE",
                "turn_id": session.turn_id,
                "outcome": outcome or "ok",
            }
            await self._send_session_event(payload, "SESSION_DONE")

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
        outcome = "ok"
        if session:
            intent = session.intent_name or ""
            outcome = "llm" if intent == "llm_reply" else ("local_intent" if intent else "ok")
        if self._store and session:
            self._store.record_turn(
                event.turn_id,
                outcome,
                transcript=session.final_text,
                reply=session.reply_text,
                route=outcome,
            )
        if session:
            session.meta["outcome"] = outcome
            await self._arm_followup_if_needed(session)
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
            try:
                await adapter.send_speech_cancel(event.turn_id)
            except Exception as exc:
                log.warning(
                    "Barge-in: SPEECH_CANCEL falhou sem bloquear novo turno: %s",
                    exc,
                )

        self._session.discard_reason = "barge_in"
        self._session.meta["outcome"] = "interrupted"

        # Cancela Task de turno (LLM stream + TTS + OutputScheduler)
        await self._cancel_current_turn(reason="barge_in")

        t_cancelled = time.monotonic()
        self._metrics.record("interruption_cancel_ms", (t_cancelled - t_barge) * 1000.0)
        log.info("Barge-in: turno %d cancelado em %.0f ms", event.turn_id, (t_cancelled - t_barge) * 1000.0)
        if self._store:
            self._store.record_turn(
                event.turn_id,
                "interrupted",
                transcript=self._session.final_text,
                reply=self._session.reply_text,
                route="barge_in",
            )

        # Restaura baseline IDLE no robô (regra de baseline do CLAUDE.md)
        await self._robot.reset_baseline(adapter, event.turn_id)

        # VAD reset — limpa contadores para o próximo turno
        self._vad.reset()

        # INTERRUPTED → LISTENING: inicia novo turno completo para capturar a nova fala.
        # Usa _begin_turn para armar watchdog, limpar VAD/STT e manter o ciclo simétrico.
        self._fsm.try_transition(TurnState.INTERRUPTED)
        self._empty_wake_prompted_at = None
        await self._begin_turn()

    async def _on_turn_error(self, event: TurnError) -> None:
        log.error("Erro no turno %d estágio=%s: %s", event.turn_id, event.stage, event.reason)
        if self._store:
            self._store.record_turn(
                event.turn_id,
                "failed",
                transcript=self._session.final_text if self._session else None,
                reply=self._session.reply_text if self._session else None,
                route=event.stage,
            )
            self._store.record_error(
                kind=f"{event.stage}_failure",
                turn_id=event.turn_id,
                message=event.reason[:200],
            )
        if self._session:
            self._session.meta["outcome"] = "failed"
            self._session.meta["error_stage"] = event.stage
            self._session.meta["error_reason"] = event.reason
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
        await self._cancel_watchdog()
        self._watchdog = asyncio.create_task(
            self._run_watchdog(self._session),
            name=f"nb_watchdog_{self._session.turn_id}",
        )
        log.info("Turno %d iniciado (LISTENING)", self._session.turn_id)

    def _max_utterance_samples(self) -> int:
        audio = getattr(self._config, "audio", None)
        value = getattr(audio, "max_utterance_samples", DEFAULT_MAX_UTTERANCE_SAMPLES)
        try:
            return max(0, int(value))
        except (TypeError, ValueError):
            return DEFAULT_MAX_UTTERANCE_SAMPLES

    async def _run_watchdog(self, session: SessionContext) -> None:
        """Cancela o turno se o deadline for excedido — Invariante I-4."""
        try:
            while True:
                await asyncio.sleep(2.0)
                if self._session is not session:
                    return   # turno terminou normalmente
                if session.is_past_deadline():
                    overdue_s = max(0.0, time.monotonic() - (session.deadline or 0.0))
                    if self._fsm.state == TurnState.LISTENING:
                        await self._expire_listening_turn(session, overdue_s)
                        return
                    log.warning(
                        "Watchdog: turno %d excedeu deadline (atraso %.1f s, estado=%s) — cancelando",
                        session.turn_id, overdue_s, self._fsm.state.name,
                    )
                    if self._store:
                        self._store.record_error(
                            kind="watchdog_timeout",
                            turn_id=session.turn_id,
                            message=f"deadline excedido em {self._fsm.state.name}",
                        )
                    await self._bus.publish(TurnError(
                        turn_id=session.turn_id,
                        stage="watchdog",
                        reason="deadline_exceeded",
                    ))
                    return
        except asyncio.CancelledError:
            pass

    async def _expire_listening_turn(
        self, session: SessionContext, overdue_s: float
    ) -> None:
        """Encerra timeout de escuta sem transformar ausência de fala em erro."""
        if self._session is not session:
            return
        reason = "listen_timeout"
        log.info(
            "Watchdog: turno %d expirou em LISTENING (atraso %.1f s, samples=%d)",
            session.turn_id,
            overdue_s,
            session.total_samples,
        )
        session.discard_reason = reason
        session.meta["outcome"] = reason
        if self._store:
            self._store.record_turn(
                session.turn_id,
                reason,
                transcript=session.final_text,
                reply=session.reply_text,
                route="watchdog",
            )
        await self._robot.reset_baseline(self.adapter, session.turn_id)
        self._fsm.try_transition(TurnState.IDLE)
        await self._finish_turn()

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
            self._session.meta.setdefault("outcome", "cancelled")
            await self._send_terminal_session_event(self._session)
            self._record_voice_session(self._session)

    async def _finish_turn(self) -> None:
        """Limpa estado de sessão após o turno terminar."""
        if self._session:
            await self._send_terminal_session_event(self._session)
            self._record_voice_session(self._session)
            log.debug("Turno %d finalizado: %s", self._session.turn_id, self._session.to_log_dict())
            snap = self._metrics.snapshot_flat()
            if snap:
                log.debug("Métricas (p50/p95): %s", {k: f"{v:.0f}ms" if v else "—" for k, v in snap.items()})
        self._session = None
        self._turn_task = None
        # Cancela watchdog — turno terminou dentro do prazo
        await self._cancel_watchdog()

    async def _cancel_watchdog(self) -> None:
        """Cancela e aguarda a task de watchdog para evitar tasks penduradas."""
        watchdog = self._watchdog
        self._watchdog = None
        if watchdog is None or watchdog.done():
            return
        if watchdog is asyncio.current_task():
            return
        watchdog.cancel()
        try:
            await watchdog
        except asyncio.CancelledError:
            pass

    def _record_voice_session(self, session: SessionContext) -> None:
        if not self._store:
            return

        timeline = session.timeline

        def delta_ms(start: str, end: str) -> float | None:
            a = timeline.get(start)
            b = timeline.get(end)
            if a is None or b is None:
                return None
            return round((b - a) * 1000.0, 1)

        def since_start_ms(name: str) -> float | None:
            value = timeline.get(name)
            if value is None:
                return None
            return round((value - session.t_start) * 1000.0, 1)

        outcome = session.meta.get("outcome")
        if not outcome:
            if session.discard_reason:
                outcome = "discarded"
            elif session.reply_text:
                outcome = "ok"
            else:
                outcome = "idle"

        self._store.record_voice_session({
            "turn_id": session.turn_id,
            "outcome": outcome,
            "state": self._fsm.state.name.lower(),
            "discard_reason": session.discard_reason,
            "voice_end_reason": session.meta.get("voice_end_reason"),
            "total_samples": session.total_samples,
            "duration_ms": round(session.duration_s() * 1000.0, 1),
            "chunk_count": len(session.audio_chunks),
            "transcript_quality": session.meta.get("transcript_quality"),
            "transcript": session.final_text,
            "no_speech_prob": session.meta.get("no_speech_prob"),
            "avg_logprob": session.meta.get("avg_logprob"),
            "compression_ratio": session.meta.get("compression_ratio"),
            "intent_name": session.intent_name,
            "reply": session.reply_text,
            "reply_chars": len(session.reply_text or ""),
            "tts_sentence_count": session.meta.get("tts_sentence_count"),
            "tts_text_chars": session.meta.get("tts_text_chars"),
            "tts_chunks_sent": session.meta.get("tts_chunks_sent"),
            "tts_pcm_bytes_in": session.meta.get("tts_pcm_bytes_in"),
            "tts_pcm_bytes_sent": session.meta.get("tts_pcm_bytes_sent"),
            "tts_padding_bytes": session.meta.get("tts_padding_bytes"),
            "tts_say_begin_sent": session.meta.get("tts_say_begin_sent"),
            "tts_say_end_sent": session.meta.get("tts_say_end_sent"),
            "tts_expected_duration_ms": session.meta.get("tts_expected_duration_ms"),
            "tts_completed": session.meta.get("tts_completed"),
            "text_scroll_chars": session.meta.get("text_scroll_chars"),
            "text_scroll_bytes": session.meta.get("text_scroll_bytes"),
            "text_scroll_payload_bytes": session.meta.get("text_scroll_payload_bytes"),
            "text_scroll_truncated": session.meta.get("text_scroll_truncated"),
            "text_scroll_pages": session.meta.get("text_scroll_pages"),
            "text_scroll_pages_sent": session.meta.get("text_scroll_pages_sent"),
            "voice_end_to_stt_start_ms": delta_ms("voice_end", "stt_start"),
            "stt_ms": delta_ms("stt_start", "stt_end"),
            "end_of_turn_ms": since_start_ms("stt_end"),
            "tts_first_audio_ms": delta_ms("tts_start", "first_audio_out"),
            "first_audio_out_ms": since_start_ms("first_audio_out"),
            "first_audio_after_voice_end_ms": delta_ms("voice_end", "first_audio_out"),
            "speech_total_ms": since_start_ms("speech_done"),
            "error_stage": session.meta.get("error_stage"),
            "error_reason": session.meta.get("error_reason"),
        })


def _split_text_scroll_pages(text: str, *, max_bytes: int = TEXT_SCROLL_MAX_BYTES) -> list[str]:
    """Split visible reply text into UTF-8-safe TEXT_SCROLL pages."""
    words = " ".join(str(text or "").split()).split(" ")
    pages: list[str] = []
    current = ""

    for word in words:
        if not word:
            continue
        candidate = f"{current} {word}".strip() if current else word
        if len(candidate.encode("utf-8")) <= max_bytes:
            current = candidate
            continue
        if current:
            pages.append(current)
            current = ""
        while len(word.encode("utf-8")) > max_bytes:
            prefix = _utf8_prefix(word, max_bytes)
            if not prefix:
                break
            pages.append(prefix)
            word = word[len(prefix) :]
        current = word

    if current:
        pages.append(current)
    return pages or []


def _utf8_prefix(text: str, max_bytes: int) -> str:
    out = ""
    for char in text:
        candidate = out + char
        if len(candidate.encode("utf-8")) > max_bytes:
            break
        out = candidate
    return out
