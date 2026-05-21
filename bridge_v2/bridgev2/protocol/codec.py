"""bridgev2.protocol.codec — FrameDecoder incremental stream-safe.

Ao contrário de decode_frames() (batch), o FrameDecoder é alimentado com
bytes à medida que chegam e produz frames completos. É o componente correto
para uso com asyncio streams (transport TCP/UART).

Implementação completa na Fase 2. Aqui está o esqueleto com interface definida.
"""
from __future__ import annotations

from typing import Iterator

from .framing import crc8, SOF, FRAME_OVERHEAD


class FrameDecoder:
    """Decoder incremental de frames do protocolo NoiseBot.

    Alimenta bytes via feed(); obtém frames completos via frames().
    Thread-safe? Não — use exclusivamente no event loop.
    """

    def __init__(self) -> None:
        self._buf = bytearray()
        self._frames_ready: list[tuple[int, bytes]] = []

    def feed(self, data: bytes) -> None:
        """Alimenta bytes brutos do transporte."""
        self._buf.extend(data)
        self._parse()

    def frames(self) -> list[tuple[int, bytes]]:
        """Retorna e esvazia a lista de frames completos prontos."""
        ready = self._frames_ready
        self._frames_ready = []
        return ready

    def __iter__(self) -> Iterator[tuple[int, bytes]]:
        yield from self.frames()

    # ── Parser interno ─────────────────────────────────────────────────────

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
                break  # frame incompleto
            msg_type = buf[i + 3]
            payload = bytes(buf[i + 4 : i + 4 + data_len])
            rx_crc = buf[i + 4 + data_len]
            exp_crc = crc8(bytes([msg_type]) + payload)
            if rx_crc == exp_crc:
                self._frames_ready.append((msg_type, payload))
            i += total
        del buf[:i]

    @property
    def buffered_bytes(self) -> int:
        return len(self._buf)
