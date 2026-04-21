#!/usr/bin/env python3
"""
NoiseBot LLM Bridge — Etapa 12.2
Roda em RPi4 ou PC na mesma rede local.
Pipeline: ESP32 mic → Whisper → Gemini Flash → Piper TTS → ESP32 speaker

Dependências:
    pip install zeroconf openai-whisper google-generativeai piper-tts numpy

Uso:
    python bridge.py [--host noisebot.local] [--port 9000]
    python bridge.py --uart  # fallback USB CDC (sem WiFi)
"""

import argparse
import socket
import struct
import threading
import time
import logging
import numpy as np
import os
import sys

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(name)s: %(message)s")
log = logging.getLogger("noisebot_bridge")

# ── Frame protocol ────────────────────────────────────────────────────────────

SOF          = 0xAB
FRAME_OVERHEAD = 5  # SOF(1)+LEN(2)+TYPE(1)+CRC(1)

# MSG types (must match nb_bridge_msg_type_t)
MSG_HELLO        = 0x00
MSG_AUDIO_CHUNK  = 0x01
MSG_EVENT        = 0x02
MSG_STATUS       = 0x03
MSG_SAY          = 0x10
MSG_EXPR         = 0x11
MSG_ACTION       = 0x12
MSG_EMOT_EVENT   = 0x13
MSG_GAZE         = 0x14
MSG_TEXT_SCROLL  = 0x15

# nb_event_type_t values — deve coincidir com nb_events.h
NB_EVT_VOICE_ACTIVITY_START = 9
NB_EVT_VOICE_ACTIVITY_END   = 10

def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) if (crc & 0x80) else (crc << 1)
            crc &= 0xFF
    return crc

def encode_frame(msg_type: int, payload: bytes = b"") -> bytes:
    length = len(payload)
    header = bytes([SOF, length & 0xFF, (length >> 8) & 0xFF, msg_type])
    crc_data = bytes([msg_type]) + payload
    return header + payload + bytes([crc8(crc_data)])

def decode_frames(buf: bytearray):
    """Yield (msg_type, payload) tuples from buf, consuming parsed bytes."""
    consumed = 0
    frames = []
    while consumed + FRAME_OVERHEAD <= len(buf):
        i = consumed
        if buf[i] != SOF:
            consumed += 1
            continue
        data_len = buf[i+1] | (buf[i+2] << 8)
        total = FRAME_OVERHEAD + data_len
        if consumed + total > len(buf):
            break
        msg_type = buf[i+3]
        payload  = bytes(buf[i+4 : i+4+data_len])
        rx_crc   = buf[i+4+data_len]
        exp_crc  = crc8(bytes([msg_type]) + payload)
        if rx_crc != exp_crc:
            log.warning("CRC error type=0x%02X — descartado", msg_type)
            consumed += total
            continue
        frames.append((msg_type, payload))
        consumed += total
    del buf[:consumed]
    return frames

# ── Transport layer ───────────────────────────────────────────────────────────

class TcpTransport:
    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port
        self.sock = None

    def connect(self, timeout=5.0) -> bool:
        try:
            self.sock = socket.create_connection((self.host, self.port), timeout=timeout)
            self.sock.settimeout(0.1)
            return True
        except Exception as e:
            log.error("TCP connect %s:%d falhou: %s", self.host, self.port, e)
            return False

    def send(self, data: bytes):
        try:
            self.sock.sendall(data)
        except Exception as e:
            log.warning("TCP send erro: %s", e)
            raise

    def recv(self, max_bytes=4096) -> bytes:
        try:
            data = self.sock.recv(max_bytes)
            if data == b"":
                raise ConnectionError("TCP peer closed")
            return data
        except socket.timeout:
            return b""
        except Exception as e:
            raise

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None


class UartTransport:
    """USB CDC fallback (serial port)."""
    def __init__(self, port: str, baud: int = 921600):
        import serial
        self.ser = serial.Serial(port, baud, timeout=0.1)

    def send(self, data: bytes):
        self.ser.write(data)

    def recv(self, max_bytes=4096) -> bytes:
        return self.ser.read(max_bytes)

    def close(self):
        self.ser.close()

# ── Handshake ─────────────────────────────────────────────────────────────────

def do_handshake(transport, timeout=1.0) -> bool:
    try:
        transport.send(encode_frame(MSG_HELLO))
    except Exception as e:
        log.warning("Handshake send falhou: %s", e)
        return False

    buf = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            data = transport.recv(FRAME_OVERHEAD)
        except Exception as e:
            log.warning("Handshake recv falhou: %s", e)
            return False
        if data:
            buf.extend(data)
        if len(buf) >= FRAME_OVERHEAD:
            frames = decode_frames(buf)
            if frames and frames[0][0] == MSG_HELLO:
                return True
    return False

# ── mDNS discovery ────────────────────────────────────────────────────────────

def discover_mdns(timeout=5.0) -> str | None:
    try:
        return socket.gethostbyname("noisebot.local")
    except Exception:
        return None

# ── LLM pipeline ─────────────────────────────────────────────────────────────

def transcribe_whisper(pcm_int16: np.ndarray) -> dict:
    import whisper
    model = transcribe_whisper._model
    audio = pcm_int16.astype(np.float32) / 32768.0
    result = model.transcribe(audio, language="pt", fp16=False)
    segs = result.get("segments", [])
    if segs:
        no_speech = max(float(s.get("no_speech_prob", 0.0)) for s in segs)
        avg_logprob = float(np.mean([float(s.get("avg_logprob", -10.0)) for s in segs]))
        compression = max(float(s.get("compression_ratio", 0.0)) for s in segs)
    else:
        no_speech = 1.0
        avg_logprob = -10.0
        compression = 999.0
    return {
        "text": result.get("text", "").strip(),
        "no_speech_prob": no_speech,
        "avg_logprob": avg_logprob,
        "compression_ratio": compression,
    }

transcribe_whisper._model = None

def init_whisper():
    import whisper
    log.info("Carregando Whisper small...")
    transcribe_whisper._model = whisper.load_model("small")
    log.info("Whisper pronto")

def ask_gemini(text: str, status: dict) -> dict:
    """Retorna dict com keys: reply, expression_id, action, emot_event."""
    import google.genai as genai
    model = ask_gemini._model
    state_desc = f"estado={status.get('state',0)} valence={status.get('valence',0):.2f}"
    prompt = (
        f"Você é NoiseBot, um companion robot expressivo de mesa. "
        f"Personalidade: caloroso, curioso, expressivo, respostas < 10s de fala. "
        f"Estado atual: {state_desc}. "
        f"Usuário disse: \"{text}\"\n"
        "Responda com JSON: {\"reply\":\"...\",\"expression_id\":0,\"action\":0,\"emot_event\":2}\n"
        "expression_id: 0=neutro,1=feliz,2=curioso,3=sonolento,4=focado,5=desconfiado\n"
        "action: 0=greet,1=nod,2=shake,3=look_up,4=look_down\n"
        "emot_event: 2=voice_start,3=audio_started (use 2 ao começar resposta)\n"
        "Responda SOMENTE com o JSON, sem markdown."
    )
    for attempt in range(3):
        try:
            response = model.models.generate_content(model="gemini-2.0-flash-lite", contents=prompt)
            import json, re
            text_resp = response.text.strip()
            m = re.search(r'\{.*\}', text_resp, re.DOTALL)
            if m:
                return json.loads(m.group())
            return {"reply": text_resp, "expression_id": 0, "action": 0, "emot_event": 2}
        except Exception as e:
            if "429" in str(e):
                log.warning("Gemini 429 — cota excedida, fale novamente em alguns segundos")
                return {"reply": "", "expression_id": 0, "action": 0, "emot_event": 2}
            raise
    return {"reply": "", "expression_id": 0, "action": 0, "emot_event": 2}

ask_gemini._model = None

def init_gemini():
    import google.genai as genai
    api_key = os.environ.get("GEMINI_API_KEY", "")
    if not api_key:
        log.warning("GEMINI_API_KEY não definida — LLM desabilitado")
        return
    client = genai.Client(api_key=api_key)
    ask_gemini._model = client
    log.info("Gemini Flash pronto")

def tts_piper(text: str) -> np.ndarray:
    """Sintetiza texto em PCM int16 16kHz via Piper TTS."""
    import subprocess, io, wave
    cmd = ["piper", "--model", os.environ.get("PIPER_MODEL", "pt_BR-faber-medium.onnx"),
           "--output_raw"]
    proc = subprocess.run(cmd, input=text.encode(), capture_output=True)
    pcm = np.frombuffer(proc.stdout, dtype=np.int16)
    return pcm

# ── Bridge session ────────────────────────────────────────────────────────────

CHUNK_SAMPLES    = 256
VOICE_TIMEOUT_S  = 25.0  # watchdog: firmware deve enviar VOICE_END antes disso
MIN_UTTERANCE_SAMPLES = 8000   # 0.5s: rejeição pré-Whisper
MIN_UTTERANCE_RMS     = 80.0   # PCM int16 pós-filtro no firmware
MAX_NO_SPEECH_PROB    = 0.75
MAX_NO_SPEECH_PROB_DRY_RUN = 0.90
MIN_AVG_LOGPROB       = -1.10
MAX_COMPRESSION_RATIO = 2.60

class BridgeSession:
    def __init__(self, transport, dry_run: bool = False):
        self.transport          = transport
        self.rx_buf             = bytearray()
        self.audio_buf          = []          # chunks PCM acumulados durante VOICE_ACTIVE
        self.streaming          = False
        self.last_status        = {}
        self._lock              = threading.Lock()
        self._voice_timer       = None
        self.dry_run            = dry_run
        self._session_start     = None   # timestamp float quando VOICE_START chega
        self._session_rms_sum   = 0.0    # soma de RMS por chunk para avg
        self._session_n_chunks  = 0      # número de chunks recebidos
        self._session_audio_seen = False # loga primeiro chunk recebido
        self._session_id        = 0

    def send_msg(self, msg_type: int, payload: bytes = b""):
        self.transport.send(encode_frame(msg_type, payload))

    def send_say_pcm(self, pcm: np.ndarray):
        """Envia PCM int16 em chunks de 256 samples via MSG_SAY."""
        for i in range(0, len(pcm), CHUNK_SAMPLES):
            chunk = pcm[i:i+CHUNK_SAMPLES]
            if len(chunk) < CHUNK_SAMPLES:
                chunk = np.pad(chunk, (0, CHUNK_SAMPLES - len(chunk)))
            self.send_msg(MSG_SAY, chunk.astype(np.int16).tobytes())
            time.sleep(0.014)  # ~14ms por chunk (256/16000 = 16ms, com margem)

    def _cancel_voice_timer(self):
        if self._voice_timer is not None:
            self._voice_timer.cancel()
            self._voice_timer = None

    def _arm_voice_timer(self):
        self._cancel_voice_timer()
        session_id = self._session_id
        self._voice_timer = threading.Timer(VOICE_TIMEOUT_S,
                                            self._on_voice_timeout,
                                            args=(session_id,))
        self._voice_timer.daemon = True
        self._voice_timer.start()

    def _on_voice_timeout(self, session_id: int):
        snapshot = self._snapshot_voice_session(session_id)
        if snapshot is not None:
            log.warning("VOICE timeout — forçando VOICE_END após %.0fs", VOICE_TIMEOUT_S)
            threading.Thread(target=self.handle_voice_end,
                             args=(snapshot,),
                             daemon=True).start()

    def _snapshot_voice_session(self, session_id: int | None = None):
        with self._lock:
            if session_id is not None and session_id != self._session_id:
                return None
            if not self.streaming and not self.audio_buf:
                return None

            self._cancel_voice_timer()
            pcm_chunks = list(self.audio_buf)
            self.audio_buf.clear()
            self.streaming = False

            session_start = self._session_start
            snapshot = {
                "audio_chunks": pcm_chunks,
                "avg_rms": self._session_rms_sum / max(1, self._session_n_chunks),
                "duration_s": time.time() - (session_start or time.time()),
                "session_id": self._session_id,
            }
            self._session_start = None
            self._session_rms_sum = 0.0
            self._session_n_chunks = 0
            self._session_audio_seen = False
            return snapshot

    def handle_voice_end(self, snapshot):
        discard_reason = None
        n_samples      = 0
        text           = ""
        rms_pcm        = 0.0
        peak           = 0
        tr             = {
            "no_speech_prob": 0.0,
            "avg_logprob": 0.0,
            "compression_ratio": 0.0,
        }
        avg_rms        = snapshot["avg_rms"]
        duration_s     = snapshot["duration_s"]
        try:
            audio_chunks = snapshot["audio_chunks"]
            if not audio_chunks:
                discard_reason = "buffer_vazio"
                return
            pcm = np.concatenate(audio_chunks).astype(np.int16)
            n_samples = len(pcm)

            if transcribe_whisper._model is None:
                discard_reason = "whisper_nao_pronto"
                return
            if not self.dry_run and ask_gemini._model is None:
                discard_reason = "gemini_nao_pronto"
                return

            if n_samples < MIN_UTTERANCE_SAMPLES:
                discard_reason = f"audio_curto_{n_samples}smp"
                return

            pcm_f = pcm.astype(np.float32)
            rms_pcm = float(np.sqrt(np.mean(pcm_f * pcm_f)))
            peak    = int(np.max(np.abs(pcm.astype(np.int32))))
            if rms_pcm < MIN_UTTERANCE_RMS and not self.dry_run:
                discard_reason = f"rms_baixo_{rms_pcm:.0f}"
                return

            tr   = transcribe_whisper(pcm)
            text = tr["text"]
            if not text:
                discard_reason = "texto_vazio"
                return

            max_no_speech = MAX_NO_SPEECH_PROB_DRY_RUN if self.dry_run else MAX_NO_SPEECH_PROB
            if tr["no_speech_prob"] > max_no_speech:
                discard_reason = f"no_speech_{tr['no_speech_prob']:.2f}"
                return
            if tr["avg_logprob"] < MIN_AVG_LOGPROB:
                discard_reason = f"logprob_{tr['avg_logprob']:.2f}"
                return
            if tr["compression_ratio"] > MAX_COMPRESSION_RATIO:
                discard_reason = f"compression_{tr['compression_ratio']:.2f}"
                return
            if self.dry_run:
                self.send_msg(MSG_SAY, b"")  # ACK silencioso: cancela timer de resposta no ESP32.
                discard_reason = "dry_run_ok"
                return

            response = ask_gemini(text, self.last_status)
            reply    = response.get("reply", "")
            expr_id  = int(response.get("expression_id", 0))
            action   = int(response.get("action", 0))
            emot_ev  = int(response.get("emot_event", 2))

            self.send_msg(MSG_EMOT_EVENT, struct.pack("<I", emot_ev))
            self.send_msg(MSG_EXPR,       struct.pack("<BI", expr_id, 4000))

            tts_pcm = tts_piper(reply)
            self.send_msg(MSG_ACTION, struct.pack("<I", 0))
            self.send_say_pcm(tts_pcm)

        except Exception as e:
            log.error("Pipeline LLM falhou: %s", e, exc_info=True)
            discard_reason = "exception"
        finally:
            outcome = "ok" if discard_reason is None else f"descartado:{discard_reason}"
            log.info(
                "SESSAO dur=%.1fs samples=%d avg_rms=%.0f pcm_rms=%.0f peak=%d "
                "ns=%.2f logp=%.2f comp=%.2f texto=%r motivo=%s",
                duration_s, n_samples, avg_rms, rms_pcm, peak,
                tr["no_speech_prob"], tr["avg_logprob"], tr["compression_ratio"],
                text or "", outcome,
            )

    def process_msg(self, msg_type: int, payload: bytes):
        if msg_type == MSG_AUDIO_CHUNK:
            with self._lock:
                streaming = self.streaming
                if streaming:
                    self.audio_buf.append(np.frombuffer(payload, dtype=np.int16).copy())
                    pcm = self.audio_buf[-1]
                    rms_c = float(np.sqrt(np.mean(pcm.astype(np.float32) ** 2)))
                    self._session_rms_sum  += rms_c
                    self._session_n_chunks += 1
                    first_audio = not self._session_audio_seen
                    if first_audio:
                        self._session_audio_seen = True
                else:
                    first_audio = False
            if streaming and first_audio:
                log.info("AUDIO primeiro chunk payload=%d bytes rms=%.0f",
                         len(payload), rms_c)

        elif msg_type == MSG_EVENT:
            if len(payload) >= 4:
                evt_type = struct.unpack_from("<I", payload)[0]
                log.info("EVENT evt_type=%d", evt_type)
                if evt_type == NB_EVT_VOICE_ACTIVITY_END:
                    log.info("VOICE_END recebido — processando")
                    snapshot = self._snapshot_voice_session()
                    if snapshot is not None:
                        threading.Thread(target=self.handle_voice_end,
                                         args=(snapshot,),
                                         daemon=True).start()
                elif evt_type == NB_EVT_VOICE_ACTIVITY_START:
                    with self._lock:
                        if self.streaming:
                            log.warning("VOICE_START recebido durante sessão ativa — resetando sessão anterior")
                        log.info("VOICE_START recebido")
                        self._cancel_voice_timer()
                        self._session_id += 1
                        self.streaming         = True
                        self.audio_buf.clear()
                        self._session_start    = time.time()
                        self._session_rms_sum  = 0.0
                        self._session_n_chunks = 0
                        self._session_audio_seen = False
                        self._arm_voice_timer()
                else:
                    log.debug("EVENT ignorado evt_type=%d", evt_type)

        elif msg_type == MSG_STATUS:
            if len(payload) >= 14:
                state, = struct.unpack_from("<B", payload, 0)
                valence, activation, attention = struct.unpack_from("<fff", payload, 1)
                health, = struct.unpack_from("<B", payload, 13)
                self.last_status = dict(state=state, valence=valence,
                                        activation=activation, attention=attention,
                                        health=health)

    def run(self):
        log.info("Sessão bridge ativa")
        total_bytes = 0
        while True:
            try:
                data = self.transport.recv(4096)
                if data:
                    total_bytes += len(data)
                    log.debug("RX %d bytes (total=%d) tipo=0x%02X",
                              len(data), total_bytes, data[0])
                    self.rx_buf.extend(data)
                frames = decode_frames(self.rx_buf)
                for msg_type, payload in frames:
                    log.debug("FRAME type=0x%02X payload=%d bytes",
                              msg_type, len(payload))
                    self.process_msg(msg_type, payload)
            except Exception as e:
                log.error("Erro de I/O: %s", e)
                self._cancel_voice_timer()
                break
        log.info("Sessão bridge encerrada")

# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="NoiseBot LLM Bridge")
    parser.add_argument("--host", default=None, help="IP ou hostname do ESP32")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--uart", default=None, metavar="PORT", help="Porta serial USB CDC")
    parser.add_argument("--dry-run", action="store_true",
                        help="Transcreve com Whisper e não chama Gemini/Piper")
    args = parser.parse_args()

    init_whisper()
    if args.dry_run:
        log.info("DRY-RUN ativo — Gemini/Piper desabilitados")
    else:
        init_gemini()

    while True:
        if args.uart:
            log.info("Conectando via UART %s", args.uart)
            transport = UartTransport(args.uart)
        else:
            host = args.host or discover_mdns() or "noisebot.local"
            log.info("Conectando TCP %s:%d", host, args.port)
            transport = TcpTransport(host, args.port)
            if not transport.connect():
                log.info("Retentando em 5s...")
                time.sleep(5)
                continue

        if not do_handshake(transport):
            log.warning("Handshake falhou — retentando")
            transport.close()
            time.sleep(2)
            continue

        log.info("Handshake OK")
        session = BridgeSession(transport, dry_run=args.dry_run)
        session.run()
        transport.close()
        log.info("Reconectando em 5s...")
        time.sleep(5)

if __name__ == "__main__":
    main()
