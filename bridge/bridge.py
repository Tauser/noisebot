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

# nb_event_type_t values (subset needed by bridge)
NB_EVT_VOICE_ACTIVITY_END = 7   # must match firmware enum

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
            return self.sock.recv(max_bytes)
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
    transport.send(encode_frame(MSG_HELLO))
    buf = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        data = transport.recv(FRAME_OVERHEAD)
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

def transcribe_whisper(pcm_int16: np.ndarray) -> str:
    import whisper
    model = transcribe_whisper._model
    audio = pcm_int16.astype(np.float32) / 32768.0
    result = model.transcribe(audio, language="pt", fp16=False)
    return result["text"].strip()

transcribe_whisper._model = None

def init_whisper():
    import whisper
    log.info("Carregando Whisper small...")
    transcribe_whisper._model = whisper.load_model("small")
    log.info("Whisper pronto")

def ask_gemini(text: str, status: dict) -> dict:
    """Retorna dict com keys: reply, expression_id, action, emot_event."""
    import google.generativeai as genai
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
    response = model.generate_content(prompt)
    import json, re
    text_resp = response.text.strip()
    m = re.search(r'\{.*\}', text_resp, re.DOTALL)
    if m:
        return json.loads(m.group())
    return {"reply": text_resp, "expression_id": 0, "action": 0, "emot_event": 2}

ask_gemini._model = None

def init_gemini():
    import google.generativeai as genai
    api_key = os.environ.get("GEMINI_API_KEY", "")
    if not api_key:
        log.warning("GEMINI_API_KEY não definida — LLM desabilitado")
        return
    genai.configure(api_key=api_key)
    ask_gemini._model = genai.GenerativeModel("gemini-1.5-flash")
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

CHUNK_SAMPLES = 512

class BridgeSession:
    def __init__(self, transport):
        self.transport = transport
        self.rx_buf    = bytearray()
        self.audio_buf = []          # chunks PCM acumulados durante VOICE_ACTIVE
        self.streaming = False
        self.last_status = {}
        self._lock = threading.Lock()

    def send_msg(self, msg_type: int, payload: bytes = b""):
        self.transport.send(encode_frame(msg_type, payload))

    def send_say_pcm(self, pcm: np.ndarray):
        """Envia PCM int16 em chunks de 512 samples via MSG_SAY."""
        for i in range(0, len(pcm), CHUNK_SAMPLES):
            chunk = pcm[i:i+CHUNK_SAMPLES]
            if len(chunk) < CHUNK_SAMPLES:
                chunk = np.pad(chunk, (0, CHUNK_SAMPLES - len(chunk)))
            self.send_msg(MSG_SAY, chunk.astype(np.int16).tobytes())
            time.sleep(0.028)  # ~28ms por chunk (512/16000 ≈ 32ms, com margem)

    def handle_voice_end(self):
        if not self.audio_buf:
            return
        pcm = np.concatenate(self.audio_buf).astype(np.int16)
        self.audio_buf.clear()
        self.streaming = False

        if transcribe_whisper._model is None or ask_gemini._model is None:
            log.warning("Whisper ou Gemini não prontos — ignorando")
            return

        try:
            text = transcribe_whisper(pcm)
            log.info("Transcrição: %s", text)
            if not text:
                return

            response = ask_gemini(text, self.last_status)
            reply    = response.get("reply", "")
            expr_id  = int(response.get("expression_id", 0))
            action   = int(response.get("action", 0))
            emot_ev  = int(response.get("emot_event", 2))

            # Envia EMOT_EVENT e EXPR antes de falar
            self.send_msg(MSG_EMOT_EVENT, struct.pack("<I", emot_ev))
            self.send_msg(MSG_EXPR,       struct.pack("<BI", expr_id, 4000))

            # TTS → SAY streaming
            tts_pcm = tts_piper(reply)
            self.send_msg(MSG_ACTION, struct.pack("<I", 0))  # NB_BRIDGE_ACTION_GREET
            self.send_say_pcm(tts_pcm)

            log.info("Resposta enviada (%d samples)", len(tts_pcm))
        except Exception as e:
            log.error("Pipeline LLM falhou: %s", e)

    def process_msg(self, msg_type: int, payload: bytes):
        if msg_type == MSG_AUDIO_CHUNK:
            if self.streaming:
                pcm = np.frombuffer(payload, dtype=np.int16)
                self.audio_buf.append(pcm)

        elif msg_type == MSG_EVENT:
            if len(payload) >= 4:
                evt_type = struct.unpack_from("<I", payload)[0]
                if evt_type == NB_EVT_VOICE_ACTIVITY_END:
                    log.info("VOICE_END recebido — processando")
                    self.streaming = False
                    threading.Thread(target=self.handle_voice_end, daemon=True).start()
                else:
                    self.streaming = True  # qualquer outro evento de voz = ativo

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
        while True:
            try:
                data = self.transport.recv(4096)
                if data:
                    self.rx_buf.extend(data)
                frames = decode_frames(self.rx_buf)
                for msg_type, payload in frames:
                    self.process_msg(msg_type, payload)
            except Exception as e:
                log.error("Erro de I/O: %s", e)
                break
        log.info("Sessão bridge encerrada")

# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="NoiseBot LLM Bridge")
    parser.add_argument("--host", default=None, help="IP ou hostname do ESP32")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--uart", default=None, metavar="PORT", help="Porta serial USB CDC")
    args = parser.parse_args()

    init_whisper()
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
        session = BridgeSession(transport)
        session.run()
        transport.close()
        log.info("Reconectando em 5s...")
        time.sleep(5)

if __name__ == "__main__":
    main()
