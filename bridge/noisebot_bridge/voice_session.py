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
from .llm import LlmResult
from .protocol import MSG_ACTION, MSG_EMOT_EVENT, MSG_EXPR, MSG_SAY, encode_frame
from .stt import SttResult

log = logging.getLogger("noisebot_bridge.session")


@dataclass
class VoiceSnapshot:
    session_id: int
    audio_chunks: list[np.ndarray]
    avg_rms: float
    duration_s: float
    end_reason: str


class VoiceSessionRuntime:
    def __init__(self, transport, stt, llm, tts, dry_run: bool = False):
        self.transport = transport
        self.stt = stt
        self.llm = llm
        self.tts = tts
        self.dry_run = dry_run
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
            threading.Thread(target=self.handle_voice_end, args=(snapshot,), daemon=True).start()

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

    def begin_voice(self):
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

    def handle_voice_end(self, snapshot: VoiceSnapshot):
        discard_reason = None
        silent_ack_sent = False
        route = "discard"
        n_samples = 0
        text = ""
        rms_pcm = 0.0
        peak = 0
        tr = self.stt.empty_result() if self.stt else SttResult()
        llm_result = LlmResult()
        avg_rms = snapshot.avg_rms
        duration_s = snapshot.duration_s
        end_reason = snapshot.end_reason
        session_id = snapshot.session_id

        def ack_once():
            nonlocal silent_ack_sent
            if not silent_ack_sent:
                self.send_silent_ack()
                silent_ack_sent = True

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
            if self.dry_run:
                discard_reason = "dry_run_ok"
                return

            if not self.llm or not self.llm.ready:
                discard_reason = "llm_indisponivel"
                ack_once()
                return

            route = "llm"
            llm_result = self.llm.generate(text, self.last_status)
            if llm_result.error:
                discard_reason = llm_result.error
                ack_once()
                return
            reply = llm_result.reply

            self.send_msg(MSG_EMOT_EVENT, struct.pack("<I", llm_result.emot_event))
            self.send_msg(MSG_EXPR, struct.pack("<BI", llm_result.expression_id, 4000))

            tts_pcm = self.tts.synthesize(reply)
            self.send_msg(MSG_ACTION, struct.pack("<I", 0))
            self.send_say_pcm(tts_pcm)

        except Exception as e:
            log.error("Pipeline LLM falhou session_id=%d: %s", session_id, e, exc_info=True)
            discard_reason = "exception"
            try:
                ack_once()
            except Exception:
                pass
        finally:
            outcome = "ok" if discard_reason is None else f"descartado:{discard_reason}"
            log_text = text or ""
            if len(log_text) > LOG_TEXT_MAX_CHARS:
                log_text = log_text[:LOG_TEXT_MAX_CHARS] + "..."
            log.info(
                "SESSAO session_id=%d route=%s dur=%.1fs samples=%d avg_rms=%.0f pcm_rms=%.0f peak=%d "
                "stt=%s/%0.fms gain=%.1f peak_in=%.3f ns=%.2f logp=%.2f comp=%.2f "
                "llm=%s/%s/%0.fms texto=%r reason=%s motivo=%s",
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
                log_text,
                end_reason,
                outcome,
            )
