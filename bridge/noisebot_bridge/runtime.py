from __future__ import annotations

import logging
import struct
import threading
import time

from .protocol import (
    MSG_HELLO,
    MSG_AUDIO_CHUNK,
    MSG_EVENT,
    MSG_STATUS,
    NB_EVT_VOICE_ACTIVITY_END,
    NB_EVT_VOICE_ACTIVITY_START,
    decode_frames,
    decode_hello_payload,
    encode_frame,
    encode_hello_payload,
)
from .voice_session import VoiceSessionRuntime

log = logging.getLogger("noisebot_bridge.runtime")


class BridgeRuntime:
    def __init__(self, transport, stt, llm, tts, dry_run: bool = False, intent_router=None):
        self.transport = transport
        self.rx_buf = bytearray()
        self.state = "idle"
        self.peer_capabilities = None
        self.voice = VoiceSessionRuntime(transport, stt, llm, tts, dry_run=dry_run, intent_router=intent_router)

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

        if msg_type == MSG_AUDIO_CHUNK:
            self.set_state("receiving_audio")
            self.voice.append_audio_chunk(payload)
            return

        if msg_type == MSG_EVENT and len(payload) >= 4:
            evt_type = struct.unpack_from("<I", payload)[0]
            log.info("EVENT evt_type=%d", evt_type)
            if evt_type == NB_EVT_VOICE_ACTIVITY_END:
                reason_code = struct.unpack_from("<I", payload, 4)[0] if len(payload) >= 8 else None
                end_reason = self.voice.voice_end_reason_name(reason_code)
                log.info("VOICE_END recebido — processando reason=%s session_id=%d", end_reason, self.voice.current_session_id)
                snapshot = self.voice.snapshot_voice_session(end_reason=end_reason)
                if snapshot is not None:
                    self.set_state("transcribing")
                    threading.Thread(target=self._handle_voice_end_thread, args=(snapshot,), daemon=True).start()
            elif evt_type == NB_EVT_VOICE_ACTIVITY_START:
                self.set_state("receiving_audio")
                self.voice.begin_voice()
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

    def _handle_voice_end_thread(self, snapshot):
        self.voice.handle_voice_end(snapshot)
        self.set_state("idle")

    def run(self):
        log.info("Sessão bridge ativa")
        try:
            self.transport.send(encode_frame(MSG_HELLO, encode_hello_payload()))
            log.debug("HELLO v2 enviado")
        except Exception as e:
            log.warning("HELLO v2 falhou — seguindo em modo v1: %s", e)
        total_bytes = 0
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
            except KeyboardInterrupt:
                log.info("Bridge interrompido por Ctrl+C")
                self.voice.cancel_voice_timer()
                raise
            except Exception as e:
                log.error("Erro de I/O: %s", e)
                self.voice.cancel_voice_timer()
                break
        log.info("Sessão bridge encerrada")
