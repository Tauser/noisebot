from __future__ import annotations

from dataclasses import dataclass
import logging
import struct
import threading
import time

import numpy as np

from .config import (
    CHUNK_SAMPLES,
    LOG_TEXT_MAX_CHARS,
    MAX_COMPRESSION_RATIO,
    MAX_NO_SPEECH_PROB,
    MAX_NO_SPEECH_PROB_DRY_RUN,
    MIN_AVG_LOGPROB,
    MIN_TRANSCRIBE_PEAK,
    MIN_TRANSCRIBE_RMS,
    MIN_UTTERANCE_RMS,
    MIN_UTTERANCE_SAMPLES,
    VOICE_TIMEOUT_S,
)
from .device_commands import DeviceCommandDispatcher
from .intent_router import DeviceCommand, LocalIntentResult
from .llm import LlmResult
from .protocol import (
    MSG_ACTION,
    MSG_EMOT_EVENT,
    MSG_EXPR,
    MSG_SAY,
    SESSION_THINKING_START,
    SESSION_TRANSCRIBE_START,
    SESSION_TTS_START,
    SESSION_TTS_STOP,
    encode_frame,
)
from .stt import SttResult

log = logging.getLogger("noisebot_bridge.session")

THINKING_EVENT_DELAY_S = 1.0


@dataclass
class VoiceSnapshot:
    session_id: int
    audio_chunks: list[np.ndarray]
    avg_rms: float
    duration_s: float
    end_reason: str


@dataclass
class VoiceSessionResult:
    session_id: int
    end_reason: str
    route: str
    outcome: str
    outcome_detail: str = "none"
    error_reason: str | None = None


def classify_session_outcome(discard_reason: str | None, route: str, end_reason: str) -> tuple[str, str, str | None]:
    if discard_reason is None:
        return "ok", "none", None
    if discard_reason == "dry_run_ok":
        return "dry_run_ok", "none", None
    if discard_reason == "exception":
        return "pipeline_error", "exception", "pipeline_error"
    if end_reason == "bridge_watchdog_timeout":
        return "session_timeout", "bridge_watchdog_timeout", "bridge_watchdog_timeout"
    if discard_reason == "whisper_nao_pronto":
        return "stt_unavailable", discard_reason, "stt_unavailable"
    if discard_reason == "llm_indisponivel":
        return "llm_unavailable", discard_reason, "llm_unavailable"
    if discard_reason in ("gemini_429", "llm_quota_exceeded"):
        return "llm_quota_exceeded", discard_reason, "llm_quota_exceeded"
    if discard_reason in ("tts_indisponivel", "tts_send_failed"):
        return "tts_failed", discard_reason, "tts_failed"
    if discard_reason.startswith(("audio_curto_", "audio_baixo_", "rms_baixo_", "buffer_vazio")):
        return "audio_rejected", discard_reason, "audio_rejected"
    if discard_reason.startswith(("texto_vazio", "no_speech_", "logprob_", "compression_")):
        return "stt_rejected", discard_reason, "stt_rejected"
    if route == "error":
        return "route_error", discard_reason, "route_error"
    if route == "llm":
        return "llm_error", discard_reason, "llm_error"
    return "discarded", discard_reason, None


class VoiceSessionRuntime:
    def __init__(
        self,
        transport,
        stt,
        llm,
        tts,
        dry_run: bool = False,
        intent_router=None,
        device_dispatcher=None,
        session_event_cb=None,
    ):
        self.transport = transport
        self.stt = stt
        self.llm = llm
        self.tts = tts
        self.dry_run = dry_run
        self.intent_router = intent_router
        self.device_dispatcher = device_dispatcher or DeviceCommandDispatcher(self.send_msg)
        self.session_event_cb = session_event_cb
        self.audio_buf: list[np.ndarray] = []
        self.streaming = False
        self.last_status: dict = {}
        self._lock = threading.Lock()
        self._voice_timer = None
        self._session_start = None
        self._session_rms_sum = 0.0
        self._session_n_chunks = 0
        self._session_audio_seen = False
        self._session_id = 0

    def emit_session_event(self, event: str, session_id: int, **fields):
        if self.session_event_cb is not None:
            self.session_event_cb(event, session_id, **fields)

    @property
    def current_session_id(self) -> int:
        return self._session_id

    def send_msg(self, msg_type: int, payload: bytes = b""):
        self.transport.send(encode_frame(msg_type, payload))

    def send_say_pcm(self, pcm: np.ndarray):
        for i in range(0, len(pcm), CHUNK_SAMPLES):
            chunk = pcm[i : i + CHUNK_SAMPLES]
            if len(chunk) < CHUNK_SAMPLES:
                chunk = np.pad(chunk, (0, CHUNK_SAMPLES - len(chunk)))
            self.send_msg(MSG_SAY, chunk.astype(np.int16).tobytes())
            time.sleep(0.014)

    def send_silent_ack(self):
        self.send_msg(MSG_SAY, b"")

    def cancel_voice_timer(self):
        if self._voice_timer is not None:
            self._voice_timer.cancel()
            self._voice_timer = None

    def arm_voice_timer(self):
        self.cancel_voice_timer()
        session_id = self._session_id
        self._voice_timer = threading.Timer(VOICE_TIMEOUT_S, self.on_voice_timeout, args=(session_id,))
        self._voice_timer.daemon = True
        self._voice_timer.start()

    def on_voice_timeout(self, session_id: int):
        snapshot = self.snapshot_voice_session(session_id, end_reason="bridge_watchdog_timeout")
        if snapshot is not None:
            log.warning("VOICE timeout — forçando VOICE_END após %.0fs session_id=%d", VOICE_TIMEOUT_S, session_id)
            threading.Thread(target=self._handle_timeout_voice_end, args=(snapshot,), daemon=True).start()

    def _handle_timeout_voice_end(self, snapshot: VoiceSnapshot):
        result = self.handle_voice_end(snapshot)
        if self.session_event_cb is not None:
            if result.error_reason is not None:
                self.session_event_cb("SESSION_ERROR", result.session_id, reason=result.error_reason, source="bridge_watchdog")
            self.session_event_cb("SESSION_DONE", result.session_id, reason=result.end_reason, source="bridge_watchdog")

    @staticmethod
    def voice_end_reason_name(reason_code: int | None) -> str:
        if reason_code == 0:
            return "silence"
        if reason_code == 1:
            return "timeout"
        if reason_code == 2:
            return "bridge_disconnected"
        if reason_code == 3:
            return "cancelled"
        return "unknown"

    def begin_voice(self) -> int:
        with self._lock:
            if self.streaming:
                log.warning("VOICE_START recebido durante sessão ativa — resetando sessão anterior")
            self.cancel_voice_timer()
            self._session_id += 1
            self.streaming = True
            self.audio_buf.clear()
            self._session_start = time.time()
            self._session_rms_sum = 0.0
            self._session_n_chunks = 0
            self._session_audio_seen = False
            session_id = self._session_id
            self.arm_voice_timer()
        log.info("VOICE_START recebido session_id=%d", session_id)
        return session_id

    def append_audio_chunk(self, payload: bytes):
        with self._lock:
            streaming = self.streaming
            if streaming:
                self.audio_buf.append(np.frombuffer(payload, dtype=np.int16).copy())
                pcm = self.audio_buf[-1]
                rms_c = float(np.sqrt(np.mean(pcm.astype(np.float32) ** 2)))
                self._session_rms_sum += rms_c
                self._session_n_chunks += 1
                first_audio = not self._session_audio_seen
                if first_audio:
                    self._session_audio_seen = True
                    session_id = self._session_id
            else:
                first_audio = False
                rms_c = 0.0
                session_id = self._session_id
        if streaming and first_audio:
            log.info("AUDIO primeiro chunk session_id=%d payload=%d bytes rms=%.0f", session_id, len(payload), rms_c)

    def snapshot_voice_session(self, session_id: int | None = None, end_reason: str = "unknown") -> VoiceSnapshot | None:
        with self._lock:
            if session_id is not None and session_id != self._session_id:
                return None
            if not self.streaming and not self.audio_buf:
                return None

            self.cancel_voice_timer()
            pcm_chunks = list(self.audio_buf)
            self.audio_buf.clear()
            self.streaming = False

            session_start = self._session_start
            snapshot = VoiceSnapshot(
                session_id=self._session_id,
                audio_chunks=pcm_chunks,
                avg_rms=self._session_rms_sum / max(1, self._session_n_chunks),
                duration_s=time.time() - (session_start or time.time()),
                end_reason=end_reason,
            )
            self._session_start = None
            self._session_rms_sum = 0.0
            self._session_n_chunks = 0
            self._session_audio_seen = False
            return snapshot

    def handle_voice_end(self, snapshot: VoiceSnapshot) -> VoiceSessionResult:
        discard_reason = None
        silent_ack_sent = False
        route = "discard"
        n_samples = 0
        text = ""
        rms_pcm = 0.0
        peak = 0
        tr = self.stt.empty_result() if self.stt else SttResult()
        llm_result = LlmResult()
        local_intent: LocalIntentResult | None = None
        intent_kind = "none"
        avg_rms = snapshot.avg_rms
        duration_s = snapshot.duration_s
        end_reason = snapshot.end_reason
        session_id = snapshot.session_id

        def ack_once():
            nonlocal silent_ack_sent
            if not silent_ack_sent:
                self.send_silent_ack()
                silent_ack_sent = True

        def speak_text(text_to_speak: str, action: int = 0) -> bool:
            nonlocal discard_reason
            self.emit_session_event(SESSION_TTS_START, session_id)
            try:
                tts_pcm = self.tts.synthesize(text_to_speak)
            except Exception as e:
                discard_reason = "tts_indisponivel"
                log.warning("TTS indisponivel session_id=%d: %s", session_id, e)
                ack_once()
                self.emit_session_event(SESSION_TTS_STOP, session_id, reason="tts_failed")
                return False
            try:
                self.send_msg(MSG_ACTION, struct.pack("<I", action))
                self.send_say_pcm(tts_pcm)
            except Exception as e:
                discard_reason = "tts_send_failed"
                log.warning("TTS envio falhou session_id=%d: %s", session_id, e)
                self.emit_session_event(SESSION_TTS_STOP, session_id, reason="tts_send_failed")
                return False
            self.emit_session_event(SESSION_TTS_STOP, session_id, reason="ok")
            return True

        def schedule_thinking_event() -> tuple[threading.Event, threading.Timer]:
            cancelled = threading.Event()

            def emit_if_pending():
                if not cancelled.is_set():
                    self.emit_session_event(SESSION_THINKING_START, session_id, source="llm")

            timer = threading.Timer(THINKING_EVENT_DELAY_S, emit_if_pending)
            timer.daemon = True
            timer.start()
            return cancelled, timer

        try:
            if not snapshot.audio_chunks:
                discard_reason = "buffer_vazio"
                return
            pcm = np.concatenate(snapshot.audio_chunks).astype(np.int16)
            n_samples = len(pcm)
            if end_reason == "timeout" and n_samples > 0:
                end_reason = "max_speech_timeout"

            if not self.stt or not self.stt.ready:
                discard_reason = "whisper_nao_pronto"
                ack_once()
                return

            if n_samples < MIN_UTTERANCE_SAMPLES:
                discard_reason = f"audio_curto_{n_samples}smp"
                ack_once()
                return

            pcm_f = pcm.astype(np.float32)
            rms_pcm = float(np.sqrt(np.mean(pcm_f * pcm_f)))
            peak = int(np.max(np.abs(pcm.astype(np.int32))))
            if rms_pcm < MIN_TRANSCRIBE_RMS or peak < MIN_TRANSCRIBE_PEAK:
                discard_reason = f"audio_baixo_rms{rms_pcm:.0f}_peak{peak}"
                ack_once()
                return
            if rms_pcm < MIN_UTTERANCE_RMS and not self.dry_run:
                discard_reason = f"rms_baixo_{rms_pcm:.0f}"
                ack_once()
                return

            if self.dry_run:
                ack_once()

            self.emit_session_event(SESSION_TRANSCRIBE_START, session_id)
            tr = self.stt.transcribe(pcm)
            text = tr.text
            if not text:
                discard_reason = "texto_vazio"
                ack_once()
                return

            max_no_speech = MAX_NO_SPEECH_PROB_DRY_RUN if self.dry_run else MAX_NO_SPEECH_PROB
            if tr.no_speech_prob > max_no_speech:
                discard_reason = f"no_speech_{tr.no_speech_prob:.2f}"
                ack_once()
                return
            if tr.avg_logprob < MIN_AVG_LOGPROB:
                discard_reason = f"logprob_{tr.avg_logprob:.2f}"
                ack_once()
                return
            if tr.compression_ratio > MAX_COMPRESSION_RATIO:
                discard_reason = f"compression_{tr.compression_ratio:.2f}"
                ack_once()
                return

            if self.intent_router is not None:
                local_intent = self.intent_router.route(text, self.last_status)
                if local_intent is not None:
                    route = "local_intent"
                    intent_kind = "device_command" if local_intent.device_commands else "local_text"
                    log.info(
                        "INTENT session_id=%d kind=%s intent=%s confidence=%.2f reply=%r",
                        session_id,
                        intent_kind,
                        local_intent.intent,
                        local_intent.confidence,
                        local_intent.reply,
                    )
                    if self.dry_run:
                        discard_reason = "dry_run_ok"
                        return
                    self.send_msg(MSG_EMOT_EVENT, struct.pack("<I", local_intent.emot_event))
                    self.send_msg(MSG_EXPR, struct.pack("<BI", local_intent.expression_id, 4000))
                    for command in local_intent.device_commands:
                        self.device_dispatcher.dispatch(command)
                    if not local_intent.speak_reply:
                        if not local_intent.device_commands and local_intent.reply:
                            self.device_dispatcher.dispatch(
                                DeviceCommand("scroll_text", {"text": local_intent.reply}, supported=True)
                            )
                        log.info("INTENT session_id=%d reply_suppressed intent=%s", session_id, local_intent.intent)
                        ack_once()
                        return
                    spoke_ok = speak_text(local_intent.reply, local_intent.action)
                    if spoke_ok and not local_intent.device_commands and local_intent.reply:
                        self.device_dispatcher.dispatch(
                            DeviceCommand("scroll_text", {"text": local_intent.reply}, supported=True)
                        )
                    return

            if self.dry_run:
                discard_reason = "dry_run_ok"
                return

            if not self.llm or not self.llm.ready:
                discard_reason = "llm_indisponivel"
                route = "error"
                if self.dry_run:
                    ack_once()
                    return
                self.send_msg(MSG_EMOT_EVENT, struct.pack("<I", 2))
                self.send_msg(MSG_EXPR, struct.pack("<BI", 2, 4000))
                speak_text("Eu entendi, mas estou sem acesso ao cérebro online agora.")
                return

            route = "llm"
            thinking_cancel, thinking_timer = schedule_thinking_event()
            try:
                llm_result = self.llm.generate(text, self.last_status)
            finally:
                thinking_cancel.set()
                thinking_timer.cancel()
            if llm_result.error:
                discard_reason = llm_result.error
                ack_once()
                return
            reply = llm_result.reply

            self.send_msg(MSG_EMOT_EVENT, struct.pack("<I", llm_result.emot_event))
            self.send_msg(MSG_EXPR, struct.pack("<BI", llm_result.expression_id, 4000))

            speak_text(reply)

        except Exception as e:
            log.error("Pipeline de voz falhou session_id=%d: %s", session_id, e, exc_info=True)
            discard_reason = "exception"
            try:
                ack_once()
            except Exception:
                pass
        finally:
            outcome, outcome_detail, error_reason = classify_session_outcome(discard_reason, route, end_reason)
            log_text = text or ""
            if len(log_text) > LOG_TEXT_MAX_CHARS:
                log_text = log_text[:LOG_TEXT_MAX_CHARS] + "..."
            log.info(
                "SESSAO session_id=%d route=%s dur=%.1fs samples=%d avg_rms=%.0f pcm_rms=%.0f peak=%d "
                "stt=%s/%0.fms gain=%.1f peak_in=%.3f ns=%.2f logp=%.2f comp=%.2f "
                "llm=%s/%s/%0.fms intent_kind=%s intent=%s texto=%r end_reason=%s outcome=%s detail=%s",
                session_id,
                route,
                duration_s,
                n_samples,
                avg_rms,
                rms_pcm,
                peak,
                tr.backend,
                tr.elapsed_ms,
                tr.gain,
                tr.peak_in,
                tr.no_speech_prob,
                tr.avg_logprob,
                tr.compression_ratio,
                llm_result.provider,
                llm_result.model,
                llm_result.elapsed_ms,
                intent_kind,
                local_intent.intent if local_intent else "none",
                log_text,
                end_reason,
                outcome,
                outcome_detail,
            )
            return VoiceSessionResult(
                session_id=session_id,
                end_reason=end_reason,
                route=route,
                outcome=outcome,
                outcome_detail=outcome_detail,
                error_reason=error_reason,
            )
