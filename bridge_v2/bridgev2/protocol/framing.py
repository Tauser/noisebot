"""bridgev2.protocol.framing — Framing 0xAB + CRC-8 (congelado).

ATENÇÃO: Este módulo é byte-compatível com o firmware. NÃO alterar o
framing, o CRC, o SOF nem a ordem dos campos sem sincronizar com o firmware.

Formato de frame:
  [0xAB][LEN_LO][LEN_HI][TYPE][DATA...][CRC8]

  SOF  = 0xAB
  LEN  = uint16 little-endian = nº de bytes de DATA (sem TYPE)
  TYPE = 1 byte
  DATA = LEN bytes (payload da mensagem)
  CRC8 = CRC-8/SMBUS sobre (TYPE + DATA)
  FRAME_OVERHEAD = 5 bytes

Reaproveita a lógica de bridge/noisebot_bridge/protocol.py — sem modificações
funcionais; apenas organizado como módulo independente.
"""
from __future__ import annotations

import logging

log = logging.getLogger(__name__)

SOF: int = 0xAB
FRAME_OVERHEAD: int = 5  # SOF(1) + LEN(2) + TYPE(1) + CRC(1)
MAX_FRAME_DATA_LEN: int = 65535


def crc8(data: bytes) -> int:
    """CRC-8/SMBUS sobre os bytes fornecidos."""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) if (crc & 0x80) else (crc << 1)
            crc &= 0xFF
    return crc


def encode_frame(msg_type: int, payload: bytes = b"") -> bytes:
    """Empacota payload em um frame do protocolo NoiseBot."""
    if len(payload) > MAX_FRAME_DATA_LEN:
        raise ValueError(f"Payload de {len(payload)} bytes excede o máximo de {MAX_FRAME_DATA_LEN}")
    length = len(payload)
    header = bytes([SOF, length & 0xFF, (length >> 8) & 0xFF, msg_type])
    crc_data = bytes([msg_type]) + payload
    return header + payload + bytes([crc8(crc_data)])


def decode_frames(buf: bytearray) -> list[tuple[int, bytes]]:
    """Decodifica todos os frames completos do buffer.

    Consome bytes processados do buffer in-place (del buf[:consumed]).
    Retorna lista de (msg_type, payload). Frames com CRC inválido são descartados.
    """
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
            break  # frame incompleto — aguardar mais dados
        msg_type = buf[i + 3]
        payload = bytes(buf[i + 4 : i + 4 + data_len])
        rx_crc = buf[i + 4 + data_len]
        exp_crc = crc8(bytes([msg_type]) + payload)
        if rx_crc != exp_crc:
            log.warning(
                "framing: CRC error type=0x%02X rx=0x%02X exp=0x%02X — frame descartado",
                msg_type, rx_crc, exp_crc,
            )
            consumed += total
            continue
        frames.append((msg_type, payload))
        consumed += total

    del buf[:consumed]
    return frames
