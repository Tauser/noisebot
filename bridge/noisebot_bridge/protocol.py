from __future__ import annotations

import logging

log = logging.getLogger("noisebot_bridge.protocol")

SOF = 0xAB
FRAME_OVERHEAD = 5  # SOF(1)+LEN(2)+TYPE(1)+CRC(1)

MSG_HELLO = 0x00
MSG_AUDIO_CHUNK = 0x01
MSG_EVENT = 0x02
MSG_STATUS = 0x03
MSG_SAY = 0x10
MSG_EXPR = 0x11
MSG_ACTION = 0x12
MSG_EMOT_EVENT = 0x13
MSG_GAZE = 0x14
MSG_TEXT_SCROLL = 0x15

NB_EVT_VOICE_ACTIVITY_START = 9
NB_EVT_VOICE_ACTIVITY_END = 10


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
