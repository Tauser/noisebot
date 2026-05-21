"""bridgev2.transport.adapter -- FirmwareAdapter: frames <-> eventos do bus.

Responsabilidades:
  RX: bytes do transporte -> FrameDecoder -> eventos tipados -> bus
  TX: eventos do bus (RobotCommand, SayChunkOut, SpeechCancel) -> frames -> transporte

Tambem realiza o handshake HELLO v2 ao conectar.
"""
from __future__ import annotations

import asyncio
import logging
import struct
import time
from typing import Any

from ..protocol.codec import FrameDecoder
from ..protocol.framing import encode_frame
from ..protocol.messages import (
    MSG_HELLO, MSG_AUDIO_CHUNK, MSG_EVENT, MSG_STATUS, MSG_SESSION,
    MSG_SAY, MSG_EXPR, MSG_ACTION, MSG_EMOT_EVENT, MSG_GAZE,
    MSG_TEXT_SCROLL, MSG_VOLUME, MSG_SPEECH_CANCEL, MSG_SAY_BEGIN, MSG_SAY_END,
    NB_EVT_VOICE_ACTIVITY_START, NB_EVT_VOICE_ACTIVITY_END,
    BRIDGE_V2_HELLO_CAPABILITIES,
    encode_hello, decode_hello, decode_event, decode_status, decode_session,
    encode_expr, encode_action, encode_emot_event, encode_gaze,
    encode_text_scroll, encode_volume, encode_speech_cancel,
    encode_say_begin, encode_say_end,
)
from ..runtime.bus import EventBus
from ..runtime.events import (
    FirmwareConnected, FirmwareDisconnected,
    WakeDetected, VoiceActivityStart, VoiceActivityEnd, VoiceEndReason,
    AudioChunkIn, StatusUpdate,
    RobotCommand, SayChunkOut, SpeechCancel,
    ShutdownRequested,
)
from .base import Transport

log = logging.getLogger(__name__)

HANDSHAKE_TIMEOUT_S = 5.0
HELLO_WAIT_TIMEOUT_S = 3.0


class FirmwareAdapter:
    """Adapta o transporte raw ao bus de eventos.

    Uma instancia por conexao ativa. Criada pelo ConnectionSupervisor ao conectar.
    Cancelada (via task.cancel()) ao desconectar ou fazer barge-in.
    """

    def __init__(
        self,
        transport: Transport,
        bus: EventBus,
        capabilities: dict[str, Any] | None = None,
    ) -> None:
        self._transport = transport
        self._bus = bus
        self._capabilities = capabilities or BRIDGE_V2_HELLO_CAPABILITIES
        self._peer_capabilities: dict[str, Any] = {}
        self._decoder = FrameDecoder()
        self._tx_queue: asyncio.Queue[bytes] = asyncio.Queue(maxsize=256)
        self._connected = False

    @property
    def peer_capabilities(self) -> dict[str, Any]:
        return self._peer_capabilities

    @property
    def peer_features(self) -> list[str]:
        return self._peer_capabilities.get("features", [])

    def peer_supports(self, feature: str) -> bool:
        return feature in self.peer_features

    # -- Ciclo principal ----------------------------------------------------

    async def run(self) -> None:
        """Faz handshake e inicia loops RX + TX. Termina quando a conexao cai."""
        rx_task: asyncio.Task | None = None
        tx_task: asyncio.Task | None = None
        try:
            await self._handshake()
            self._connected = True
            await self._bus.publish(
                FirmwareConnected(peer_capabilities=self._peer_capabilities)
            )
            rx_task = asyncio.create_task(self._rx_loop(), name="nb_fw_rx")
            tx_task = asyncio.create_task(self._tx_loop(), name="nb_fw_tx")
            done, pending = await asyncio.wait(
                {rx_task, tx_task},
                return_when=asyncio.FIRST_COMPLETED,
            )
            for t in pending:
                t.cancel()
                try:
                    await t
                except asyncio.CancelledError:
                    pass
            for t in done:
                if not t.cancelled() and t.exception():
                    raise t.exception()  # type: ignore[misc]
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            log.warning("FirmwareAdapter: erro na conexao: %s", exc)
            raise
        finally:
            self._connected = False
            # Cancelar inner tasks em qualquer caminho de saida (inclusive CancelledError)
            for t in (rx_task, tx_task):
                if t is not None and not t.done():
                    t.cancel()
            if rx_task is not None or tx_task is not None:
                tasks = [t for t in (rx_task, tx_task) if t is not None]
                try:
                    await asyncio.gather(*tasks, return_exceptions=True)
                except asyncio.CancelledError:
                    pass
            await self._bus.publish(
                FirmwareDisconnected(reason="disconnected")
            )

    # -- Handshake ----------------------------------------------------------

    async def _handshake(self) -> None:
        """Envia HELLO v2 e aguarda HELLO do firmware."""
        hello_frame = encode_frame(MSG_HELLO, encode_hello(self._capabilities))
        await self._transport.send(hello_frame)
        log.debug("Handshake: HELLO enviado")

        decoder = FrameDecoder()
        deadline = time.monotonic() + HANDSHAKE_TIMEOUT_S
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            try:
                data = await asyncio.wait_for(
                    self._transport.recv(4096),
                    timeout=min(remaining, HELLO_WAIT_TIMEOUT_S),
                )
            except asyncio.TimeoutError:
                raise TimeoutError("Handshake: timeout aguardando HELLO do firmware")
            if not data:
                raise ConnectionError("Handshake: firmware desconectou")
            decoder.feed(data)
            for msg_type, payload in decoder.frames():
                if msg_type == MSG_HELLO:
                    try:
                        self._peer_capabilities = decode_hello(payload)
                    except ValueError as e:
                        log.warning("Handshake: HELLO invalido: %s", e)
                        self._peer_capabilities = {}
                    log.info(
                        "Handshake: firmware HELLO recebido. version=%s features=%s",
                        self._peer_capabilities.get("version"),
                        self._peer_capabilities.get("features", []),
                    )
                    return
                else:
                    log.debug(
                        "Handshake: frame inesperado type=0x%02X antes do HELLO",
                        msg_type,
                    )
        raise TimeoutError("Handshake: timeout -- HELLO do firmware nao recebido")

    # -- Loop RX ------------------------------------------------------------

    async def _rx_loop(self) -> None:
        """Le bytes do transporte, decodifica frames e publica eventos no bus."""
        log.debug("FirmwareAdapter: RX loop iniciado")
        while True:
            data = await self._transport.recv(4096)
            if not data:
                log.info("FirmwareAdapter: EOF -- encerrando RX loop")
                break
            self._decoder.feed(data)
            for msg_type, payload in self._decoder.frames():
                await self._dispatch_rx(msg_type, payload)

    async def _dispatch_rx(self, msg_type: int, payload: bytes) -> None:
        """Converte um frame recebido em evento do bus."""
        try:
            if msg_type == MSG_AUDIO_CHUNK:
                await self._bus.publish(AudioChunkIn(pcm=payload))

            elif msg_type == MSG_EVENT:
                evt_type, _data = decode_event(payload)
                if evt_type == NB_EVT_VOICE_ACTIVITY_START:
                    await self._bus.publish(VoiceActivityStart())
                elif evt_type == NB_EVT_VOICE_ACTIVITY_END:
                    reason_byte = _data[0] if _data else 0
                    try:
                        reason = VoiceEndReason(reason_byte)
                    except ValueError:
                        reason = VoiceEndReason.SILENCE
                    await self._bus.publish(VoiceActivityEnd(reason=reason))
                else:
                    log.debug("RX: MSG_EVENT desconhecido evt_type=%d", evt_type)

            elif msg_type == MSG_STATUS:
                s = decode_status(payload)
                await self._bus.publish(
                    StatusUpdate(
                        state=s["state"],
                        valence=s["valence"],
                        activation=s["activation"],
                        attention=s["attention"],
                        health=s["health"],
                    )
                )

            elif msg_type == MSG_SESSION:
                sess = decode_session(payload)
                log.debug("RX: SESSION event=%s session_id=%s",
                          sess.get("event"), sess.get("session_id"))

            elif msg_type == MSG_HELLO:
                log.info("RX: HELLO recebido apos handshake (re-negociacao)")
                try:
                    self._peer_capabilities = decode_hello(payload)
                except ValueError:
                    pass

            else:
                log.debug("RX: TYPE desconhecido 0x%02X len=%d", msg_type, len(payload))

        except Exception:
            log.exception("RX dispatch error type=0x%02X", msg_type)

    # -- Loop TX ------------------------------------------------------------

    async def _tx_loop(self) -> None:
        """Drena a fila de saida e envia frames para o firmware."""
        log.debug("FirmwareAdapter: TX loop iniciado")
        try:
            while True:
                frame = await self._tx_queue.get()
                try:
                    await self._transport.send(frame)
                except Exception as e:
                    log.warning("TX: erro ao enviar frame: %s", e)
                    self._tx_queue.task_done()
                    break
                self._tx_queue.task_done()
        except asyncio.CancelledError:
            pass

    # -- API de envio -------------------------------------------------------

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

    async def send_volume(self, percent: int) -> None:
        await self._enqueue(encode_frame(MSG_VOLUME, encode_volume(percent)))

    async def send_say(self, pcm: bytes) -> None:
        """Envia chunk PCM (int16, <=256 amostras) para o speaker do firmware."""
        await self._enqueue(encode_frame(MSG_SAY, pcm))

    async def send_say_begin(self, turn_id: int) -> None:
        if self.peer_supports("turn_id"):
            await self._enqueue(encode_frame(MSG_SAY_BEGIN, encode_say_begin(turn_id)))

    async def send_say_end(self, turn_id: int) -> None:
        if self.peer_supports("turn_id"):
            await self._enqueue(encode_frame(MSG_SAY_END, encode_say_end(turn_id)))

    async def send_speech_cancel(self, turn_id: int) -> None:
        if self.peer_supports("barge_in"):
            await self._enqueue(encode_frame(MSG_SPEECH_CANCEL, encode_speech_cancel(turn_id)))

    async def _enqueue(self, frame: bytes) -> None:
        try:
            self._tx_queue.put_nowait(frame)
        except asyncio.QueueFull:
            log.warning("TX: fila cheia -- frame descartado")
