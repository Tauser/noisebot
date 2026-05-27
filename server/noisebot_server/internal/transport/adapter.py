"""Firmware adapter: transport frames to runtime events."""

from __future__ import annotations

import asyncio
import logging
import time
from dataclasses import dataclass
from typing import Any

from ..agent.runtime import (
    AudioChunkIn,
    EventBus,
    FirmwareConnected,
    FirmwareDisconnected,
    StatusUpdate,
    VoiceActivityEnd,
    VoiceActivityStart,
    VoiceEndReason,
)
from .base import Transport
from .protocol import (
    MSG_ACTION,
    MSG_AUDIO_CHUNK,
    MSG_EMOT_EVENT,
    MSG_EVENT,
    MSG_EXPR,
    MSG_GAZE,
    MSG_HELLO,
    MSG_SAY,
    MSG_SAY_BEGIN,
    MSG_SAY_END,
    MSG_SESSION,
    MSG_SPEECH_CANCEL,
    MSG_STATUS,
    MSG_TEXT_SCROLL,
    MSG_VOLUME,
    NB_EVT_VOICE_ACTIVITY_END,
    NB_EVT_VOICE_ACTIVITY_START,
    FrameDecoder,
    SERVER_HELLO_CAPABILITIES,
    decode_event,
    decode_hello,
    decode_session,
    decode_status,
    encode_action,
    encode_emot_event,
    encode_expr,
    encode_frame,
    encode_gaze,
    encode_hello,
    encode_say_begin,
    encode_say_end,
    encode_session,
    encode_speech_cancel,
    encode_text_scroll,
    encode_volume,
)

log = logging.getLogger(__name__)

HANDSHAKE_TIMEOUT_S = 5.0
HELLO_WAIT_TIMEOUT_S = 3.0
TX_ENQUEUE_TIMEOUT_S = 2.0
TX_SEND_TIMEOUT_S = 5.0
HEARTBEAT_INTERVAL_S = 30.0
HEARTBEAT_TIMEOUT_S = 90.0
SPEECH_FRAME_TYPES = frozenset({MSG_SAY, MSG_SAY_BEGIN, MSG_SAY_END})


@dataclass
class _TxItem:
    frame: bytes
    ack: asyncio.Future[None]


class FirmwareAdapter:
    """Adapt a raw firmware transport to server runtime events."""

    def __init__(
        self,
        transport: Transport,
        bus: EventBus,
        capabilities: dict[str, Any] | None = None,
    ) -> None:
        self._transport = transport
        self._bus = bus
        self._capabilities = capabilities or SERVER_HELLO_CAPABILITIES
        self._peer_capabilities: dict[str, Any] = {}
        self._decoder = FrameDecoder()
        self._tx_queue: asyncio.Queue[_TxItem] = asyncio.Queue(maxsize=256)
        self._connected = False
        self._last_rx_mono = 0.0

    @property
    def peer_capabilities(self) -> dict[str, Any]:
        return self._peer_capabilities

    @property
    def peer_features(self) -> list[str]:
        features = self._peer_capabilities.get("features", [])
        return features if isinstance(features, list) else []

    @property
    def is_connected(self) -> bool:
        if not (self._connected and self._transport.is_connected):
            return False
        if self._last_rx_mono <= 0.0:
            return False
        return (time.monotonic() - self._last_rx_mono) <= HEARTBEAT_TIMEOUT_S

    def peer_supports(self, feature: str) -> bool:
        return feature in self.peer_features

    async def run(self) -> None:
        """Handshake, then run RX, TX and heartbeat loops until disconnect."""
        rx_task: asyncio.Task[None] | None = None
        tx_task: asyncio.Task[None] | None = None
        hb_task: asyncio.Task[None] | None = None
        try:
            await self._handshake()
            self._connected = True
            self._last_rx_mono = time.monotonic()
            await self._bus.publish(
                FirmwareConnected(peer_capabilities=self._peer_capabilities)
            )
            rx_task = asyncio.create_task(self._rx_loop(), name="nb_fw_rx")
            tx_task = asyncio.create_task(self._tx_loop(), name="nb_fw_tx")
            hb_task = asyncio.create_task(self._heartbeat_loop(), name="nb_fw_hb")
            done, pending = await asyncio.wait(
                {rx_task, tx_task, hb_task},
                return_when=asyncio.FIRST_COMPLETED,
            )
            for task in pending:
                task.cancel()
                try:
                    await task
                except asyncio.CancelledError:
                    pass
            for task in done:
                if not task.cancelled() and task.exception():
                    raise task.exception()
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            log.warning("FirmwareAdapter: erro na conexao: %s", exc)
            raise
        finally:
            self._connected = False
            for task in (rx_task, tx_task, hb_task):
                if task is not None and not task.done():
                    task.cancel()
            tasks = [task for task in (rx_task, tx_task, hb_task) if task is not None]
            if tasks:
                try:
                    await asyncio.gather(*tasks, return_exceptions=True)
                except asyncio.CancelledError:
                    pass
            await self._bus.publish(FirmwareDisconnected(reason="disconnected"))

    async def _handshake(self) -> None:
        """Perform the v1-compatible HELLO handshake, then announce v2 caps."""
        await self._transport.send(encode_frame(MSG_HELLO))
        log.debug("Handshake: HELLO compat enviado")

        decoder = FrameDecoder()
        deadline = time.monotonic() + HANDSHAKE_TIMEOUT_S
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            try:
                data = await asyncio.wait_for(
                    self._transport.recv(4096),
                    timeout=min(remaining, HELLO_WAIT_TIMEOUT_S),
                )
            except asyncio.TimeoutError as exc:
                raise TimeoutError(
                    "Handshake: timeout aguardando HELLO do firmware"
                ) from exc
            if not data:
                raise ConnectionError("Handshake: firmware desconectou")
            decoder.feed(data)
            for msg_type, payload in decoder.frames():
                if msg_type != MSG_HELLO:
                    log.debug(
                        "Handshake: frame inesperado type=0x%02X antes do HELLO",
                        msg_type,
                    )
                    continue
                try:
                    self._peer_capabilities = decode_hello(payload)
                except ValueError as exc:
                    log.warning("Handshake: HELLO invalido: %s", exc)
                    self._peer_capabilities = {}
                log.info(
                    "Handshake: firmware HELLO recebido. version=%s features=%s",
                    self._peer_capabilities.get("version"),
                    self._peer_capabilities.get("features", []),
                )
                await self._transport.send(
                    encode_frame(MSG_HELLO, encode_hello(self._capabilities))
                )
                log.debug("Handshake: capabilities v2 enviadas apos HELLO compat")
                return
        raise TimeoutError("Handshake: timeout -- HELLO do firmware nao recebido")

    async def _rx_loop(self) -> None:
        log.debug("FirmwareAdapter: RX loop iniciado")
        while True:
            data = await self._transport.recv(4096)
            if not data:
                log.info("FirmwareAdapter: EOF -- encerrando RX loop")
                break
            self._decoder.feed(data)
            for msg_type, payload in self._decoder.frames():
                self._last_rx_mono = time.monotonic()
                await self._dispatch_rx(msg_type, payload)

    async def _dispatch_rx(self, msg_type: int, payload: bytes) -> None:
        try:
            if msg_type == MSG_AUDIO_CHUNK:
                audio_caps = self._peer_capabilities.get("audio", {})
                if not isinstance(audio_caps, dict):
                    audio_caps = SERVER_HELLO_CAPABILITIES["audio"]
                chunk_samples = int(
                    audio_caps.get(
                        "chunk_samples",
                        SERVER_HELLO_CAPABILITIES["audio"]["chunk_samples"],
                    )
                )
                expected_len = chunk_samples * 2
                if len(payload) != expected_len:
                    log.warning(
                        "RX: AUDIO_CHUNK invalido len=%d esperado=%d -- descartado",
                        len(payload),
                        expected_len,
                    )
                    return
                await self._bus.publish(AudioChunkIn(pcm=payload))
            elif msg_type == MSG_EVENT:
                evt_type, data = decode_event(payload)
                await self._dispatch_event(evt_type, data)
            elif msg_type == MSG_STATUS:
                status = decode_status(payload)
                await self._bus.publish(
                    StatusUpdate(
                        state=status["state"],
                        valence=status["valence"],
                        activation=status["activation"],
                        attention=status["attention"],
                        health=status["health"],
                    )
                )
            elif msg_type == MSG_SESSION:
                session = decode_session(payload)
                log.debug(
                    "RX: SESSION event=%s session_id=%s",
                    session.get("event"),
                    session.get("session_id"),
                )
            elif msg_type == MSG_HELLO:
                log.debug("RX: HELLO recebido apos handshake")
                try:
                    self._peer_capabilities = decode_hello(payload)
                except ValueError:
                    pass
            else:
                log.debug("RX: TYPE desconhecido 0x%02X len=%d", msg_type, len(payload))
        except Exception:
            log.exception("RX dispatch error type=0x%02X", msg_type)

    async def _dispatch_event(self, evt_type: int, data: bytes) -> None:
        if evt_type == NB_EVT_VOICE_ACTIVITY_START:
            await self._bus.publish(VoiceActivityStart())
        elif evt_type == NB_EVT_VOICE_ACTIVITY_END:
            reason_byte = data[0] if data else 0
            try:
                reason = VoiceEndReason(reason_byte)
            except ValueError:
                reason = VoiceEndReason.SILENCE
            await self._bus.publish(VoiceActivityEnd(reason=reason))
        else:
            log.debug("RX: MSG_EVENT desconhecido evt_type=%d", evt_type)

    async def _heartbeat_loop(self) -> None:
        try:
            while True:
                await asyncio.sleep(HEARTBEAT_INTERVAL_S)
                await self._enqueue(
                    encode_frame(MSG_HELLO, encode_hello(self._capabilities))
                )
                rx_age = time.monotonic() - self._last_rx_mono
                if rx_age > HEARTBEAT_TIMEOUT_S:
                    self._connected = False
                    raise ConnectionError(
                        f"heartbeat sem resposta do firmware ha {rx_age:.1f}s"
                    )
        except asyncio.CancelledError:
            pass

    async def _tx_loop(self) -> None:
        log.debug("FirmwareAdapter: TX loop iniciado")
        try:
            while True:
                item = await self._tx_queue.get()
                try:
                    await asyncio.wait_for(
                        self._transport.send(item.frame),
                        timeout=TX_SEND_TIMEOUT_S,
                    )
                except Exception as exc:
                    log.warning("TX: erro ao enviar frame: %s", exc)
                    self._connected = False
                    if not item.ack.done():
                        item.ack.set_exception(ConnectionError("TX: falha ao enviar"))
                    self._tx_queue.task_done()
                    break
                if not item.ack.done():
                    item.ack.set_result(None)
                self._tx_queue.task_done()
        except asyncio.CancelledError:
            self._fail_pending_tx("TX: conexao cancelada")
        finally:
            self._fail_pending_tx("TX: loop encerrado")

    async def send_expr(self, expression_id: int, duration_ms: int = 2000) -> None:
        await self._enqueue(encode_frame(MSG_EXPR, encode_expr(expression_id, duration_ms)))

    async def send_action(self, action_id: int) -> None:
        await self._enqueue(encode_frame(MSG_ACTION, encode_action(action_id)))

    async def send_emot_event(self, event_id: int) -> None:
        await self._enqueue(encode_frame(MSG_EMOT_EVENT, encode_emot_event(event_id)))

    async def send_gaze(self, x: float, y: float) -> None:
        await self._enqueue(encode_frame(MSG_GAZE, encode_gaze(x, y)))

    async def send_text_scroll(self, text: str) -> None:
        await self._enqueue(encode_frame(MSG_TEXT_SCROLL, encode_text_scroll(text)))

    async def send_session(self, payload: dict[str, Any]) -> None:
        await self._enqueue(encode_frame(MSG_SESSION, encode_session(payload)))

    async def send_volume(self, percent: int) -> None:
        await self._enqueue(encode_frame(MSG_VOLUME, encode_volume(percent)))

    async def send_say(self, pcm: bytes) -> None:
        await self._enqueue(encode_frame(MSG_SAY, pcm))

    async def send_say_begin(self, turn_id: int) -> None:
        if self.peer_supports("turn_id"):
            await self._enqueue(encode_frame(MSG_SAY_BEGIN, encode_say_begin(turn_id)))

    async def send_say_end(self, turn_id: int) -> None:
        if self.peer_supports("turn_id"):
            await self._enqueue(encode_frame(MSG_SAY_END, encode_say_end(turn_id)))

    async def send_speech_cancel(self, turn_id: int) -> None:
        self._drop_pending_speech_frames("SPEECH_CANCEL")
        await self._enqueue(
            encode_frame(MSG_SPEECH_CANCEL, encode_speech_cancel(turn_id))
        )

    async def _enqueue(self, frame: bytes) -> None:
        if not self._connected:
            raise ConnectionError("FirmwareAdapter: desconectado")
        loop = asyncio.get_running_loop()
        item = _TxItem(frame=frame, ack=loop.create_future())
        try:
            await asyncio.wait_for(self._tx_queue.put(item), timeout=TX_ENQUEUE_TIMEOUT_S)
        except asyncio.TimeoutError as exc:
            log.warning(
                "TX: fila cheia por %.1fs -- frame descartado len=%d qsize=%d",
                TX_ENQUEUE_TIMEOUT_S,
                len(frame),
                self._tx_queue.qsize(),
            )
            raise asyncio.QueueFull from exc
        await item.ack

    def _fail_pending_tx(self, reason: str) -> None:
        while True:
            try:
                item = self._tx_queue.get_nowait()
            except asyncio.QueueEmpty:
                break
            if not item.ack.done():
                item.ack.set_exception(ConnectionError(reason))
            self._tx_queue.task_done()

    def _drop_pending_speech_frames(self, reason: str) -> None:
        kept: list[_TxItem] = []
        dropped = 0
        while True:
            try:
                item = self._tx_queue.get_nowait()
            except asyncio.QueueEmpty:
                break
            if _frame_type(item.frame) in SPEECH_FRAME_TYPES:
                dropped += 1
                if not item.ack.done():
                    item.ack.set_exception(ConnectionError(reason))
            else:
                kept.append(item)
            self._tx_queue.task_done()
        for item in kept:
            self._tx_queue.put_nowait(item)
        if dropped:
            log.info("TX: %s removeu %d frame(s) de fala pendente(s)", reason, dropped)


def _frame_type(frame: bytes) -> int | None:
    if len(frame) < 4 or frame[0] != 0xAB:
        return None
    return frame[3]


__all__ = ["FirmwareAdapter"]
