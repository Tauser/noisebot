from __future__ import annotations

import logging
import struct
import threading
import time

from .protocol import (
    MSG_HELLO,
    MSG_AUDIO_CHUNK,
    MSG_EVENT,
    MSG_SPEECH_CANCEL,
    MSG_SESSION,
    MSG_STATUS,
    NB_EVT_VOICE_ACTIVITY_END,
    NB_EVT_VOICE_ACTIVITY_START,
    SESSION_ABORT_SPEAKING,
    SESSION_LISTEN_START,
    SESSION_LISTEN_STOP,
    SESSION_SESSION_ERROR,
    SESSION_SESSION_DONE,
    SESSION_WAKE_DETECTED,
    decode_frames,
    decode_hello_payload,
    decode_session_payload,
    encode_frame,
    encode_hello_payload,
    encode_session_payload,
)
from .voice_session import VoiceSessionRuntime

log = logging.getLogger("noisebot_bridge.runtime")

BRIDGE_HEARTBEAT_S = 30.0


class BridgeRuntime:
    def __init__(self, transport, stt, llm, tts, dry_run: bool = False, intent_router=None):
        self.transport = transport
        self.rx_buf = bytearray()
        self.state = "idle"
        self.peer_capabilities = None
        self.voice = VoiceSessionRuntime(
            transport,
            stt,
            llm,
            tts,
            dry_run=dry_run,
            intent_router=intent_router,
            session_event_cb=self.log_session_event,
        )

    def set_state(self, state: str):
        if self.state != state:
            log.debug("runtime state %s -> %s", self.state, state)
            self.state = state

    def process_msg(self, msg_type: int, payload: bytes):
        if msg_type == MSG_HELLO:
            try:
                self.peer_capabilities = decode_hello_payload(payload)
            except ValueError as exc:
                log.warning("HELLO v2 invalido — ignorando: %s", exc)
                return
            log.info(
                "PROTO peer role=%s version=%s features=%s",
                self.peer_capabilities.get("role", "unknown"),
                self.peer_capabilities.get("version", 1),
                ",".join(self.peer_capabilities.get("features", [])),
            )
            return

        if msg_type == MSG_SESSION:
            try:
                event = decode_session_payload(payload)
            except ValueError as exc:
                log.warning("SESSION v2 invalido — ignorando: %s", exc)
                return
            log.info(
                "SESSION_EVENT event=%s session_id=%d reason=%s source=%s",
                event.get("event"),
                event.get("session_id"),
                event.get("reason", "none"),
                event.get("source", "firmware"),
            )
            return

        if msg_type == MSG_AUDIO_CHUNK:
            if self.voice.streaming:
                self.set_state("receiving_audio")
                self.voice.append_audio_chunk(payload)
            else:
                log.debug("AUDIO_CHUNK fora de sessão ativa — ignorando")
            return

        if msg_type == MSG_EVENT and len(payload) >= 4:
            evt_type = struct.unpack_from("<I", payload)[0]
            log.info("EVENT evt_type=%d", evt_type)
            if evt_type == NB_EVT_VOICE_ACTIVITY_END:
                if not self.voice.streaming and not self.voice.audio_buf:
                    log.debug("VOICE_END fora de sessão ativa — ignorando")
                    return
                reason_code = struct.unpack_from("<I", payload, 4)[0] if len(payload) >= 8 else None
                end_reason = self.voice.voice_end_reason_name(reason_code)
                log.info("VOICE_END recebido — processando reason=%s session_id=%d", end_reason, self.voice.current_session_id)
                self.log_session_event(SESSION_LISTEN_STOP, self.voice.current_session_id, reason=end_reason)
                snapshot = self.voice.snapshot_voice_session(end_reason=end_reason)
                if snapshot is not None:
                    self.set_state("transcribing")
                    threading.Thread(target=self._handle_voice_end_thread, args=(snapshot,), daemon=True).start()
            elif evt_type == NB_EVT_VOICE_ACTIVITY_START:
                if self.state != "idle":
                    self.cancel_active_speech("voice_start")
                self.set_state("receiving_audio")
                session_id = self.voice.begin_voice()
                self.log_session_event(SESSION_WAKE_DETECTED, session_id, source="voice_start")
                self.log_session_event(SESSION_LISTEN_START, session_id, source="voice_start")
            else:
                log.debug("EVENT ignorado evt_type=%d", evt_type)
            return

        if msg_type == MSG_STATUS and len(payload) >= 14:
            state, = struct.unpack_from("<B", payload, 0)
            valence, activation, attention = struct.unpack_from("<fff", payload, 1)
            health, = struct.unpack_from("<B", payload, 13)
            self.voice.last_status = {
                "state": state,
                "valence": valence,
                "activation": activation,
                "attention": attention,
                "health": health,
            }

    def log_session_event(self, event: str, session_id: int, **fields):
        reason = fields.get("reason", "none")
        source = fields.get("source", "bridge")
        log.info("SESSION_EVENT event=%s session_id=%d reason=%s source=%s", event, session_id, reason, source)
        try:
            payload = encode_session_payload(event, session_id, source=source, reason=reason)
            self.transport.send(encode_frame(MSG_SESSION, payload))
        except Exception as exc:
            log.warning("SESSION v2 envio falhou event=%s session_id=%d: %s", event, session_id, exc)

    def cancel_active_speech(self, reason: str):
        session_id = self.voice.current_session_id
        if session_id <= 0:
            return
        self.log_session_event(SESSION_ABORT_SPEAKING, session_id, reason=reason)
        try:
            self.transport.send(encode_frame(MSG_SPEECH_CANCEL, struct.pack("<I", session_id)))
        except Exception as exc:
            log.warning("SPEECH_CANCEL envio falhou session_id=%d: %s", session_id, exc)

    def _handle_voice_end_thread(self, snapshot):
        result = self.voice.handle_voice_end(snapshot)
        if result.error_reason is not None:
            self.log_session_event(SESSION_SESSION_ERROR, result.session_id, reason=result.error_reason)
        self.log_session_event(SESSION_SESSION_DONE, result.session_id, reason=result.end_reason)
        self.set_state("idle")

    def run(self):
        log.info("Sessão bridge ativa")
        try:
            self.transport.send(encode_frame(MSG_HELLO, encode_hello_payload()))
            log.debug("HELLO v2 enviado")
        except Exception as e:
            log.warning("HELLO v2 falhou — seguindo em modo v1: %s", e)
        total_bytes = 0
        next_heartbeat = time.monotonic() + BRIDGE_HEARTBEAT_S
        while True:
            try:
                data = self.transport.recv(4096)
                if data:
                    total_bytes += len(data)
                    log.debug("RX %d bytes (total=%d) tipo=0x%02X", len(data), total_bytes, data[0])
                    self.rx_buf.extend(data)
                frames = decode_frames(self.rx_buf)
                for msg_type, payload in frames:
                    log.debug("FRAME type=0x%02X payload=%d bytes", msg_type, len(payload))
                    self.process_msg(msg_type, payload)
                now = time.monotonic()
                if now >= next_heartbeat:
                    self.transport.send(encode_frame(MSG_HELLO, encode_hello_payload()))
                    next_heartbeat = now + BRIDGE_HEARTBEAT_S
            except KeyboardInterrupt:
                log.info("Bridge interrompido por Ctrl+C")
                self.voice.cancel_voice_timer()
                raise
            except Exception as e:
                log.error("Erro de I/O: %s", e)
                if self.voice.current_session_id > 0:
                    self.log_session_event(
                        SESSION_SESSION_ERROR,
                        self.voice.current_session_id,
                        reason="transport_io_error",
                    )
                self.voice.cancel_voice_timer()
                break
        log.info("Sessão bridge encerrada")
