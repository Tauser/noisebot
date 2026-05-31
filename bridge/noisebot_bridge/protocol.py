from __future__ import annotations

import json
import logging
from typing import Any

log = logging.getLogger("noisebot_bridge.protocol")

SOF = 0xAB
FRAME_OVERHEAD = 5  # SOF(1)+LEN(2)+TYPE(1)+CRC(1)
PROTOCOL_NAME = "noisebot-bridge"
PROTOCOL_VERSION = 2

MSG_HELLO = 0x00
MSG_AUDIO_CHUNK = 0x01
MSG_EVENT = 0x02
MSG_STATUS = 0x03
MSG_SESSION = 0x04
MSG_SAY = 0x10
MSG_EXPR = 0x11
MSG_ACTION = 0x12
MSG_EMOT_EVENT = 0x13
MSG_GAZE = 0x14
MSG_TEXT_SCROLL = 0x15
MSG_VOLUME = 0x16
MSG_SPEECH_CANCEL = 0x20

NB_EVT_VOICE_ACTIVITY_START = 9
NB_EVT_VOICE_ACTIVITY_END = 10

BRIDGE_HELLO_CAPABILITIES = {
    "protocol": PROTOCOL_NAME,
    "version": PROTOCOL_VERSION,
    "role": "bridge",
    "audio": {
        "format": "pcm16",
        "sample_rate": 16000,
        "channels": 1,
        "chunk_samples": 256,
    },
    "codecs": {
        "pcm16": True,
        "opus": False,
    },
    "codec_options": {
        "opus_tx": True,
        "opus_default": False,
        "opus_sample_rate": 16000,
        "opus_channels": 1,
        "opus_frame_duration": 60,
        "opus_frame_samples": 960,
        "opus_bitrate": 32000,
    },
    "conversation": {
        "auto": True,
        "manual": False,
        "followup": False,
        "realtime": False,
    },
    "audio_processor": {
        "afe_opt_in": True,
        "afe_default": False,
        "aec_supported": False,
        "device_aec": False,
    },
    "rx": ["audio_chunk", "event", "status", "hello", "session"],
    "tx": [
        "say",
        "expr",
        "action",
        "emot_event",
        "gaze",
        "text_scroll",
        "volume",
        "hello",
        "session",
        "speech_cancel",
    ],
    "features": ["local_intents", "device_commands", "session_metrics", "session_events_v2"],
}

SESSION_WAKE_DETECTED = "WAKE_DETECTED"
SESSION_LISTEN_START = "LISTEN_START"
SESSION_LISTEN_STOP = "LISTEN_STOP"
SESSION_TRANSCRIBE_START = "TRANSCRIBE_START"
SESSION_THINKING_START = "THINKING_START"
SESSION_TTS_START = "TTS_START"
SESSION_TTS_STOP = "TTS_STOP"
SESSION_SPEAK_START = "SPEAK_START"
SESSION_SPEAK_STOP = "SPEAK_STOP"
SESSION_ABORT_SPEAKING = "ABORT_SPEAKING"
SESSION_FOLLOWUP_ARM = "FOLLOWUP_ARM"
SESSION_FOLLOWUP_CANCEL = "FOLLOWUP_CANCEL"
SESSION_SESSION_DONE = "SESSION_DONE"
SESSION_SESSION_ERROR = "SESSION_ERROR"


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


def encode_hello_payload(capabilities: dict[str, Any] | None = None) -> bytes:
    caps = capabilities or BRIDGE_HELLO_CAPABILITIES
    return json.dumps(caps, separators=(",", ":"), ensure_ascii=True).encode("utf-8")


def decode_hello_payload(payload: bytes) -> dict[str, Any]:
    if not payload:
        return {"protocol": PROTOCOL_NAME, "version": 1, "role": "unknown"}
    try:
        decoded = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("HELLO payload invalido") from exc
    if not isinstance(decoded, dict):
        raise ValueError("HELLO payload deve ser objeto JSON")
    if decoded.get("protocol") != PROTOCOL_NAME:
        raise ValueError("HELLO payload de protocolo desconhecido")
    version = decoded.get("version")
    if not isinstance(version, int) or version < 1:
        raise ValueError("HELLO payload sem versao valida")
    return decoded


def validate_pcm16_audio_contract(capabilities: dict[str, Any]) -> None:
    audio = capabilities.get("audio")
    codecs = capabilities.get("codecs", {"pcm16": True, "opus": False})
    if not isinstance(audio, dict):
        raise ValueError("HELLO sem contrato de audio")
    if audio.get("format") != "pcm16":
        raise ValueError("HELLO audio.format precisa ser pcm16")
    if audio.get("sample_rate") != 16000:
        raise ValueError("HELLO audio.sample_rate precisa ser 16000")
    if audio.get("channels") != 1:
        raise ValueError("HELLO audio.channels precisa ser 1")
    if audio.get("chunk_samples") != 256:
        raise ValueError("HELLO audio.chunk_samples precisa ser 256")
    if not isinstance(codecs, dict) or codecs.get("pcm16") is not True:
        raise ValueError("HELLO precisa anunciar codec pcm16")
    if codecs.get("opus") is True:
        raise ValueError("HELLO anunciou opus antes de suporte habilitado")


def encode_session_payload(event: str, session_id: int, **fields: Any) -> bytes:
    payload = {"event": event, "session_id": session_id, **fields}
    return json.dumps(payload, separators=(",", ":"), ensure_ascii=True).encode("utf-8")


def decode_session_payload(payload: bytes) -> dict[str, Any]:
    try:
        decoded = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("SESSION payload invalido") from exc
    if not isinstance(decoded, dict):
        raise ValueError("SESSION payload deve ser objeto JSON")
    event = decoded.get("event")
    session_id = decoded.get("session_id")
    if not isinstance(event, str) or not event:
        raise ValueError("SESSION payload sem evento valido")
    if not isinstance(session_id, int) or session_id < 0:
        raise ValueError("SESSION payload sem session_id valido")
    return decoded


def decode_frames(buf: bytearray) -> list[tuple[int, bytes]]:
    consumed = 0
    frames: list[tuple[int, bytes]] = []
    while consumed + FRAME_OVERHEAD <= len(buf):
        i = consumed
        if buf[i] != SOF:
            consumed += 1
            continue
        data_len = buf[i + 1] | (buf[i + 2] << 8)
        total = FRAME_OVERHEAD + data_len
        if consumed + total > len(buf):
            break
        msg_type = buf[i + 3]
        payload = bytes(buf[i + 4 : i + 4 + data_len])
        rx_crc = buf[i + 4 + data_len]
        exp_crc = crc8(bytes([msg_type]) + payload)
        if rx_crc != exp_crc:
            log.warning("CRC error type=0x%02X — descartado", msg_type)
            consumed += total
            continue
        frames.append((msg_type, payload))
        consumed += total
    del buf[:consumed]
    return frames
