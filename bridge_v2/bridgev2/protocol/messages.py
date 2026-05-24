"""bridgev2.protocol.messages — Constantes de tipo e encode/decode por mensagem.

Mantém paridade byte a byte com o firmware (nb_bridge_protocol.h).
Extensões v2 (TYPE ≥ 0x20) são retrocompatíveis: o firmware atual ignora
TYPEs desconhecidos com segurança.
"""
from __future__ import annotations

import json
import struct
from typing import Any

# ── TYPEs base (idênticos ao firmware) ────────────────────────────────────

MSG_HELLO: int = 0x00       # fw↔bridge: capabilities JSON
MSG_AUDIO_CHUNK: int = 0x01 # fw→bridge: int16[256] PCM 16 kHz mono
MSG_EVENT: int = 0x02       # fw→bridge: uint32 evt_type + 8B dados
MSG_STATUS: int = 0x03      # fw→bridge: state(1) + valence(f32) + activation(f32) + attention(f32) + health(1)
MSG_SESSION: int = 0x04     # fw↔bridge: JSON de evento de sessão

MSG_SAY: int = 0x10         # bridge→fw: int16[≤256] PCM
MSG_EXPR: int = 0x11        # bridge→fw: uint8 expression_id + uint32 duration_ms
MSG_ACTION: int = 0x12      # bridge→fw: uint32 action_id
MSG_EMOT_EVENT: int = 0x13  # bridge→fw: uint32 nb_emotion_event
MSG_GAZE: int = 0x14        # bridge→fw: float x + float y ∈ [-1, 1]
MSG_TEXT_SCROLL: int = 0x15 # bridge→fw: UTF-8 ≤128 B
MSG_VOLUME: int = 0x16      # bridge→fw: uint8 0..100

# ── Extensões v2 (retrocompatíveis, negociadas via HELLO.features) ─────────

MSG_SPEECH_CANCEL: int = 0x20  # bridge→fw: uint32 turn_id
MSG_SAY_BEGIN: int = 0x21      # bridge→fw: uint32 turn_id + uint32 sample_rate
MSG_SAY_END: int = 0x22        # bridge→fw: uint32 turn_id

# ── Eventos MSG_EVENT do firmware ─────────────────────────────────────────

NB_EVT_VOICE_ACTIVITY_START: int = 9
NB_EVT_VOICE_ACTIVITY_END: int = 10

# ── Protocolo HELLO ────────────────────────────────────────────────────────

PROTOCOL_NAME: str = "noisebot-bridge"
PROTOCOL_VERSION: int = 2

BRIDGE_V2_HELLO_CAPABILITIES: dict[str, Any] = {
    "protocol": PROTOCOL_NAME,
    "version": PROTOCOL_VERSION,
    "role": "bridge_v2",
    "audio": {
        "format": "pcm16",
        "sample_rate": 16000,
        "channels": 1,
        "chunk_samples": 256,
    },
    "rx": ["audio_chunk", "event", "status", "hello", "session"],
    "tx": ["say", "expr", "action", "emot_event", "gaze", "text_scroll", "volume", "hello", "session"],
    # features v2 anunciadas — firmware ignora as que não conhece
    "features": [
        "local_intents",
        "device_commands",
        "session_metrics",
        "session_events_v2",
        "barge_in",
        "turn_id",
        "stt_partial",
    ],
}


# ── Encode helpers ─────────────────────────────────────────────────────────


def encode_hello(capabilities: dict[str, Any] | None = None) -> bytes:
    caps = capabilities if capabilities is not None else BRIDGE_V2_HELLO_CAPABILITIES
    return json.dumps(caps, separators=(",", ":"), ensure_ascii=True).encode("utf-8")


def encode_expr(expression_id: int, duration_ms: int = 2000) -> bytes:
    return struct.pack("<BI", expression_id & 0xFF, duration_ms)


def encode_action(action_id: int) -> bytes:
    return struct.pack("<I", action_id)


def encode_emot_event(event_id: int) -> bytes:
    return struct.pack("<I", event_id)


def encode_gaze(x: float, y: float) -> bytes:
    return struct.pack("<ff", x, y)


def encode_text_scroll(text: str) -> bytes:
    return text.encode("utf-8")[:128]


def encode_volume(percent: int) -> bytes:
    return bytes([max(0, min(100, percent))])


def encode_say_begin(turn_id: int, sample_rate: int = 16000) -> bytes:
    return struct.pack("<II", turn_id, sample_rate)


def encode_say_end(turn_id: int) -> bytes:
    return struct.pack("<I", turn_id)


def encode_speech_cancel(turn_id: int) -> bytes:
    return struct.pack("<I", turn_id)


def encode_session(payload: dict[str, Any]) -> bytes:
    return json.dumps(payload, separators=(",", ":"), ensure_ascii=True).encode("utf-8")[:256]


# ── Decode helpers ─────────────────────────────────────────────────────────


def decode_hello(payload: bytes) -> dict[str, Any]:
    if not payload:
        return {"protocol": PROTOCOL_NAME, "version": 1, "role": "unknown"}
    decoded = json.loads(payload.decode("utf-8"))
    if not isinstance(decoded, dict):
        raise ValueError("HELLO payload deve ser objeto JSON")
    return decoded


def decode_event(payload: bytes) -> tuple[int, bytes]:
    """Retorna (evt_type: uint32, data: 8 bytes)."""
    if len(payload) < 4:
        raise ValueError(f"MSG_EVENT payload curto: {len(payload)} bytes")
    evt_type = struct.unpack_from("<I", payload, 0)[0]
    data = payload[4:12].ljust(8, b"\x00")
    return evt_type, data


def decode_status(payload: bytes) -> dict[str, Any]:
    """Decodifica MSG_STATUS: state(1) + valence(f32) + activation(f32) + attention(f32) + health(1)."""
    if len(payload) < 14:
        raise ValueError(f"MSG_STATUS payload curto: {len(payload)} bytes")
    state = payload[0]
    valence, activation, attention = struct.unpack_from("<fff", payload, 1)
    health = payload[13]
    return {
        "state": state,
        "valence": valence,
        "activation": activation,
        "attention": attention,
        "health": health,
    }


def decode_session(payload: bytes) -> dict[str, Any]:
    decoded = json.loads(payload.decode("utf-8"))
    if not isinstance(decoded, dict):
        raise ValueError("SESSION payload deve ser objeto JSON")
    return decoded
