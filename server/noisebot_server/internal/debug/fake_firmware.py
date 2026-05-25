"""Fake ESP32 firmware simulator for server tests and debug tools.

Implementa o servidor TCP :9000 com o protocolo correto.
Permite testar o pipeline completo sem hardware.

Uso:
    fw = FakeFirmware(port=9001)
    async with fw.running():
        assert await fw.wait_connected(timeout=2.0)
        await fw.send_voice_start()
        ...
"""
from __future__ import annotations

import asyncio
import logging
import struct
import time
from contextlib import asynccontextmanager
from dataclasses import dataclass, field
from typing import AsyncIterator

from ..transport.protocol import (
    MSG_AUDIO_CHUNK,
    MSG_EVENT,
    MSG_HELLO,
    MSG_STATUS,
    NB_EVT_VOICE_ACTIVITY_END,
    NB_EVT_VOICE_ACTIVITY_START,
    PROTOCOL_NAME,
    FrameDecoder,
    decode_hello,
    encode_frame,
)

log = logging.getLogger(__name__)

# Capabilities que o firmware "anuncia"
FIRMWARE_HELLO = {
    "protocol": PROTOCOL_NAME,
    "version": 2,
    "role": "firmware",
    "audio": {"format": "pcm16", "sample_rate": 16000, "channels": 1, "chunk_samples": 256},
    "features": [],  # firmware base: sem extensoes v2 por padrao
}

CHUNK_SAMPLES = 256
SAMPLE_RATE = 16000


@dataclass
class ReceivedFrame:
    msg_type: int
    payload: bytes
    t: float = field(default_factory=time.monotonic)


class FakeFirmware:
    """Servidor TCP que simula o firmware do ESP32.

    - Aceita uma conexao por vez
    - Faz handshake HELLO automaticamente
    - Fornece metodos para injetar eventos (voice_start, audio_chunk, etc.)
    - Registra todos os frames recebidos do bridge para assertions
    """

    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int = 9001,
        firmware_features: list[str] | None = None,
    ) -> None:
        self._host = host
        self._port = port
        self._firmware_caps = dict(FIRMWARE_HELLO)
        if firmware_features is not None:
            self._firmware_caps = dict(FIRMWARE_HELLO)
            self._firmware_caps["features"] = firmware_features

        self._server: asyncio.Server | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._reader: asyncio.StreamReader | None = None
        self._connected_event = asyncio.Event()
        self._received: list[ReceivedFrame] = []
        self._rx_task: asyncio.Task | None = None
        self._bridge_caps: dict = {}

    # -- Ciclo de vida -------------------------------------------------------

    async def start(self) -> None:
        self._server = await asyncio.start_server(
            self._handle_connection, self._host, self._port
        )
        log.debug("FakeFirmware: ouvindo em %s:%d", self._host, self._port)

    async def stop(self) -> None:
        if self._rx_task:
            self._rx_task.cancel()
            try:
                await self._rx_task
            except asyncio.CancelledError:
                pass
        if self._writer:
            try:
                self._writer.close()
                await self._writer.wait_closed()
            except Exception:
                pass
        if self._server:
            self._server.close()
            await self._server.wait_closed()
        self._writer = None
        self._reader = None

    @asynccontextmanager
    async def running(self) -> AsyncIterator["FakeFirmware"]:
        await self.start()
        try:
            yield self
        finally:
            await self.stop()

    # -- Handler de conexao --------------------------------------------------

    async def _handle_connection(
        self,
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
    ) -> None:
        peer = writer.get_extra_info("peername")
        log.debug("FakeFirmware: bridge conectou de %s", peer)
        self._reader = reader
        self._writer = writer
        self._received.clear()

        await self._do_handshake(reader, writer)
        self._connected_event.set()

        self._rx_task = asyncio.create_task(
            self._rx_loop(reader), name="fake_fw_rx"
        )
        try:
            await self._rx_task
        except asyncio.CancelledError:
            pass
        log.debug("FakeFirmware: bridge desconectou")
        self._connected_event.clear()

    async def _do_handshake(
        self,
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
    ) -> None:
        """Aguarda HELLO do bridge, depois envia HELLO do firmware."""
        import json
        decoder = FrameDecoder()
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            try:
                data = await asyncio.wait_for(reader.read(4096), timeout=1.0)
            except asyncio.TimeoutError:
                continue
            if not data:
                raise ConnectionError("FakeFirmware: bridge desconectou no handshake")
            decoder.feed(data)
            for msg_type, payload in decoder.frames():
                if msg_type == MSG_HELLO:
                    try:
                        self._bridge_caps = decode_hello(payload)
                    except ValueError:
                        self._bridge_caps = {}
                    log.debug(
                        "FakeFirmware: HELLO do bridge recebido. features=%s",
                        self._bridge_caps.get("features", []),
                    )
                    fw_hello = json.dumps(
                        self._firmware_caps, separators=(",", ":"), ensure_ascii=True
                    ).encode()
                    writer.write(encode_frame(MSG_HELLO, fw_hello))
                    await writer.drain()
                    log.debug("FakeFirmware: HELLO enviado")
                    return
        raise TimeoutError("FakeFirmware: timeout aguardando HELLO do bridge")

    async def _rx_loop(self, reader: asyncio.StreamReader) -> None:
        decoder = FrameDecoder()
        while True:
            try:
                data = await asyncio.wait_for(reader.read(4096), timeout=0.1)
            except asyncio.TimeoutError:
                continue
            if not data:
                break
            decoder.feed(data)
            for msg_type, payload in decoder.frames():
                self._received.append(ReceivedFrame(msg_type=msg_type, payload=payload))
                log.debug(
                    "FakeFirmware: recebeu type=0x%02X len=%d", msg_type, len(payload)
                )

    # -- API de injecao de eventos -------------------------------------------

    async def send_voice_start(self) -> None:
        """Injeta NB_EVT_VOICE_ACTIVITY_START."""
        payload = struct.pack("<II", NB_EVT_VOICE_ACTIVITY_START, 0) + b"\x00" * 4
        await self._send(encode_frame(MSG_EVENT, payload))

    async def send_voice_end(self, reason: int = 0) -> None:
        """Injeta NB_EVT_VOICE_ACTIVITY_END com reason code."""
        payload = struct.pack("<II", NB_EVT_VOICE_ACTIVITY_END, 0) + bytes([reason]) + b"\x00" * 3
        await self._send(encode_frame(MSG_EVENT, payload))

    async def send_audio_chunk(self, samples: int = CHUNK_SAMPLES) -> None:
        """Injeta um AUDIO_CHUNK de silencio (zeros int16)."""
        pcm = bytes(samples * 2)  # int16 zeros
        await self._send(encode_frame(MSG_AUDIO_CHUNK, pcm))

    async def send_audio_chunks(self, count: int, samples_each: int = CHUNK_SAMPLES) -> None:
        for _ in range(count):
            await self.send_audio_chunk(samples_each)

    async def send_status(
        self,
        state: int = 0,
        valence: float = 0.0,
        activation: float = 0.0,
        attention: float = 0.0,
        health: int = 0,
    ) -> None:
        payload = struct.pack("<Bfff", state, valence, activation, attention)
        payload += bytes([health])
        await self._send(encode_frame(MSG_STATUS, payload))

    # -- Helpers e assertions ------------------------------------------------

    async def wait_connected(self, timeout: float = 3.0) -> bool:
        try:
            await asyncio.wait_for(self._connected_event.wait(), timeout=timeout)
            return True
        except asyncio.TimeoutError:
            return False

    def received_types(self) -> list[int]:
        return [f.msg_type for f in self._received]

    def received_of_type(self, msg_type: int) -> list[ReceivedFrame]:
        return [f for f in self._received if f.msg_type == msg_type]

    def clear_received(self) -> None:
        self._received.clear()

    @property
    def is_connected(self) -> bool:
        return self._connected_event.is_set()

    async def _send(self, frame: bytes) -> None:
        if self._writer is None or self._writer.is_closing():
            raise ConnectionError("FakeFirmware: nao conectado")
        self._writer.write(frame)
        await self._writer.drain()

    @property
    def bridge_capabilities(self) -> dict:
        return self._bridge_caps
