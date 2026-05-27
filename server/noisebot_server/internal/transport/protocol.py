"""NoiseBot firmware wire protocol.

This module owns the byte-level framing used by the server. It intentionally
stays byte-compatible with the firmware protocol while keeping implementation
local to the server transport boundary.
"""

from __future__ import annotations

import json
import logging
import struct
from collections.abc import Iterator
from typing import Any

log = logging.getLogger(__name__)

SOF: int = 0xAB
FRAME_OVERHEAD: int = 5
MAX_FRAME_DATA_LEN: int = 65535

MSG_HELLO: int = 0x00
MSG_AUDIO_CHUNK: int = 0x01
MSG_EVENT: int = 0x02
MSG_STATUS: int = 0x03
MSG_SESSION: int = 0x04

MSG_SAY: int = 0x10
MSG_EXPR: int = 0x11
MSG_ACTION: int = 0x12
MSG_EMOT_EVENT: int = 0x13
MSG_GAZE: int = 0x14
MSG_TEXT_SCROLL: int = 0x15
MSG_VOLUME: int = 0x16

MSG_SPEECH_CANCEL: int = 0x20
MSG_SAY_BEGIN: int = 0x21
MSG_SAY_END: int = 0x22

NB_EVT_VOICE_ACTIVITY_START: int = 9
NB_EVT_VOICE_ACTIVITY_END: int = 10

PROTOCOL_NAME: str = "noisebot-bridge"
PROTOCOL_VERSION: int = 2

SERVER_HELLO_CAPABILITIES: dict[str, Any] = {
    "protocol": PROTOCOL_NAME,
    "version": PROTOCOL_VERSION,
    "role": "server",
    "audio": {
        "format": "pcm16",
        "sample_rate": 16000,
        "channels": 1,
        "chunk_samples": 256,
    },
    "listen": {
        "mode": "auto",
        "max_speech_ms": 9200,
        "min_utterance_samples": 8000,
        "max_utterance_samples": 192000,
        "end_silence_ms": 900,
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
    ],
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


def crc8(data: bytes) -> int:
    """Return CRC-8/SMBUS for ``data``."""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) if (crc & 0x80) else (crc << 1)
            crc &= 0xFF
    return crc


def encode_frame(msg_type: int, payload: bytes = b"") -> bytes:
    """Pack a NoiseBot protocol frame."""
    if len(payload) > MAX_FRAME_DATA_LEN:
        raise ValueError(
            f"Payload de {len(payload)} bytes excede o maximo de {MAX_FRAME_DATA_LEN}"
        )
    length = len(payload)
    header = bytes([SOF, length & 0xFF, (length >> 8) & 0xFF, msg_type])
    crc_data = bytes([msg_type]) + payload
    return header + payload + bytes([crc8(crc_data)])


def decode_frames(buf: bytearray) -> list[tuple[int, bytes]]:
    """Decode complete frames from ``buf`` and consume processed bytes."""
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
            log.warning(
                "framing: CRC error type=0x%02X rx=0x%02X exp=0x%02X",
                msg_type,
                rx_crc,
                exp_crc,
            )
            consumed += total
            continue
        frames.append((msg_type, payload))
        consumed += total

    del buf[:consumed]
    return frames


class FrameDecoder:
    """Incremental frame decoder for stream transports."""

    def __init__(self) -> None:
        self._buf = bytearray()
        self._frames_ready: list[tuple[int, bytes]] = []

    def feed(self, data: bytes) -> None:
        self._buf.extend(data)
        self._parse()

    def frames(self) -> list[tuple[int, bytes]]:
        ready = self._frames_ready
        self._frames_ready = []
        return ready

    def __iter__(self) -> Iterator[tuple[int, bytes]]:
        yield from self.frames()

    @property
    def buffered_bytes(self) -> int:
        return len(self._buf)

    def _parse(self) -> None:
        buf = self._buf
        i = 0
        while i + FRAME_OVERHEAD <= len(buf):
            if buf[i] != SOF:
                i += 1
                continue
            if i + 3 >= len(buf):
                break
            data_len = buf[i + 1] | (buf[i + 2] << 8)
            total = FRAME_OVERHEAD + data_len
            if i + total > len(buf):
                break
            msg_type = buf[i + 3]
            payload = bytes(buf[i + 4 : i + 4 + data_len])
            rx_crc = buf[i + 4 + data_len]
            exp_crc = crc8(bytes([msg_type]) + payload)
            if rx_crc == exp_crc:
                self._frames_ready.append((msg_type, payload))
            i += total
        del buf[:i]


def encode_hello(capabilities: dict[str, Any] | None = None) -> bytes:
    caps = capabilities if capabilities is not None else SERVER_HELLO_CAPABILITIES
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
    raw = text.encode("utf-8")[:128]
    return raw.decode("utf-8", "ignore").encode("utf-8")


def encode_volume(percent: int) -> bytes:
    return bytes([max(0, min(100, percent))])


def encode_say_begin(turn_id: int, sample_rate: int = 16000) -> bytes:
    return struct.pack("<II", turn_id, sample_rate)


def encode_say_end(turn_id: int) -> bytes:
    return struct.pack("<I", turn_id)


def encode_speech_cancel(turn_id: int) -> bytes:
    return struct.pack("<I", turn_id)


def encode_session(payload: dict[str, Any]) -> bytes:
    return json.dumps(payload, separators=(",", ":"), ensure_ascii=True).encode("utf-8")[
        :256
    ]


def decode_hello(payload: bytes) -> dict[str, Any]:
    if not payload:
        return {"protocol": PROTOCOL_NAME, "version": 1, "role": "unknown"}
    decoded = json.loads(payload.decode("utf-8"))
    if not isinstance(decoded, dict):
        raise ValueError("HELLO payload deve ser objeto JSON")
    return decoded


def decode_event(payload: bytes) -> tuple[int, bytes]:
    if len(payload) < 4:
        raise ValueError(f"MSG_EVENT payload curto: {len(payload)} bytes")
    evt_type = struct.unpack_from("<I", payload, 0)[0]
    data = payload[4:12].ljust(8, b"\x00")
    return evt_type, data


def decode_status(payload: bytes) -> dict[str, Any]:
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


__all__ = [
    "FRAME_OVERHEAD",
    "FrameDecoder",
    "MAX_FRAME_DATA_LEN",
    "MSG_ACTION",
    "MSG_AUDIO_CHUNK",
    "MSG_EMOT_EVENT",
    "MSG_EVENT",
    "MSG_EXPR",
    "MSG_GAZE",
    "MSG_HELLO",
    "MSG_SAY",
    "MSG_SAY_BEGIN",
    "MSG_SAY_END",
    "MSG_SESSION",
    "MSG_SPEECH_CANCEL",
    "MSG_STATUS",
    "MSG_TEXT_SCROLL",
    "MSG_VOLUME",
    "NB_EVT_VOICE_ACTIVITY_END",
    "NB_EVT_VOICE_ACTIVITY_START",
    "PROTOCOL_NAME",
    "PROTOCOL_VERSION",
    "SERVER_HELLO_CAPABILITIES",
    "SOF",
    "crc8",
    "decode_event",
    "decode_frames",
    "decode_hello",
    "decode_session",
    "decode_status",
    "encode_action",
    "encode_emot_event",
    "encode_expr",
    "encode_frame",
    "encode_gaze",
    "encode_hello",
    "encode_say_begin",
    "encode_say_end",
    "encode_session",
    "encode_speech_cancel",
    "encode_text_scroll",
    "encode_volume",
]
