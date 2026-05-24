"""Testes de integracao: FirmwareAdapter + FakeFirmware (Fase 2).

Criterio Fase 2: 5 conexoes/reconexoes seguidas sem erro;
zero frames corrompidos (CRC); paridade byte a byte com o firmware.

Todos os testes usam asyncio_mode="auto" (pytest-asyncio 0.23.8).
"""
from __future__ import annotations

import asyncio
import pytest

from bridgev2.debug.fake_firmware import FakeFirmware
from bridgev2.transport.tcp import TcpTransport
from bridgev2.transport.adapter import FirmwareAdapter
from bridgev2.transport.reconnect import ConnectionSupervisor
from bridgev2.runtime.bus import EventBus
from bridgev2.runtime.events import (
    FirmwareConnected, FirmwareDisconnected,
    VoiceActivityStart, VoiceActivityEnd, AudioChunkIn, StatusUpdate,
)
from bridgev2.config import ReconnectConfig
from bridgev2.protocol.messages import MSG_HELLO, MSG_SPEECH_CANCEL, decode_hello

_PORT_BASE = 19100


def _port(offset: int) -> int:
    return _PORT_BASE + offset


def _tcp_factory(port: int):
    def factory():
        return TcpTransport(host="127.0.0.1", port=port, connect_timeout=2.0)
    return factory


def _fast_reconnect() -> ReconnectConfig:
    return ReconnectConfig(delay_s=0.05, max_delay_s=0.2)


# ── Handshake basico ──────────────────────────────────────────────────────────

class TestHandshake:
    async def test_hello_exchange(self, bus: EventBus):
        """Bridge e firmware trocam HELLO; FirmwareConnected chega no bus."""
        port = _port(1)
        fw = FakeFirmware(port=port)
        async with fw.running():
            transport = TcpTransport(host="127.0.0.1", port=port, connect_timeout=2.0)
            await transport.connect()
            adapter = FirmwareAdapter(transport, bus)
            task = asyncio.create_task(adapter.run(), name="test_handshake")

            q = bus.subscribe(FirmwareConnected)
            assert await fw.wait_connected(timeout=2.0)
            event = await asyncio.wait_for(q.get(), timeout=2.0)
            assert isinstance(event, FirmwareConnected)
            assert isinstance(event.peer_capabilities, dict)

            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
            await transport.disconnect()

    async def test_bridge_hello_received_by_firmware(self, bus: EventBus):
        """Firmware recebe HELLO compat e depois capabilities v2 do bridge."""
        port = _port(2)
        fw = FakeFirmware(port=port)
        async with fw.running():
            transport = TcpTransport(host="127.0.0.1", port=port, connect_timeout=2.0)
            await transport.connect()
            adapter = FirmwareAdapter(transport, bus)
            task = asyncio.create_task(adapter.run(), name="test_hello_rx")

            assert await fw.wait_connected(timeout=2.0)
            caps = fw.bridge_capabilities
            assert caps.get("version") == 1

            for _ in range(20):
                hello_frames = fw.received_of_type(MSG_HELLO)
                if hello_frames:
                    break
                await asyncio.sleep(0.01)

            hello_frames = fw.received_of_type(MSG_HELLO)
            assert hello_frames, "capabilities v2 nao recebidas apos HELLO compat"
            caps = decode_hello(hello_frames[-1].payload)
            assert caps.get("protocol") == "noisebot-bridge"
            assert "features" in caps

            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
            await transport.disconnect()

    async def test_peer_capabilities_stored(self, bus: EventBus):
        """adapter.peer_capabilities reflete features anunciadas pelo firmware."""
        port = _port(3)
        fw = FakeFirmware(port=port, firmware_features=["barge_in", "turn_id"])
        async with fw.running():
            transport = TcpTransport(host="127.0.0.1", port=port, connect_timeout=2.0)
            await transport.connect()
            adapter = FirmwareAdapter(transport, bus)
            task = asyncio.create_task(adapter.run(), name="test_caps")

            q = bus.subscribe(FirmwareConnected)
            await asyncio.wait_for(q.get(), timeout=2.0)

            assert adapter.peer_supports("barge_in")
            assert adapter.peer_supports("turn_id")
            assert not adapter.peer_supports("stt_partial")

            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
            await transport.disconnect()


# ── Recepcao de eventos ───────────────────────────────────────────────────────

class TestEventReception:
    async def test_voice_activity_start(self, bus: EventBus):
        port = _port(10)
        fw = FakeFirmware(port=port)
        async with fw.running():
            transport = TcpTransport(host="127.0.0.1", port=port, connect_timeout=2.0)
            await transport.connect()
            adapter = FirmwareAdapter(transport, bus)
            task = asyncio.create_task(adapter.run())

            q_voice = bus.subscribe(VoiceActivityStart)
            assert await fw.wait_connected(timeout=2.0)
            await asyncio.sleep(0.05)

            await fw.send_voice_start()
            event = await asyncio.wait_for(q_voice.get(), timeout=2.0)
            assert isinstance(event, VoiceActivityStart)

            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
            await transport.disconnect()

    async def test_voice_activity_end(self, bus: EventBus):
        port = _port(11)
        fw = FakeFirmware(port=port)
        async with fw.running():
            transport = TcpTransport(host="127.0.0.1", port=port, connect_timeout=2.0)
            await transport.connect()
            adapter = FirmwareAdapter(transport, bus)
            task = asyncio.create_task(adapter.run())

            q_end = bus.subscribe(VoiceActivityEnd)
            assert await fw.wait_connected(timeout=2.0)
            await asyncio.sleep(0.05)

            await fw.send_voice_end(reason=0)
            event = await asyncio.wait_for(q_end.get(), timeout=2.0)
            assert isinstance(event, VoiceActivityEnd)

            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
            await transport.disconnect()

    async def test_audio_chunk_received(self, bus: EventBus):
        port = _port(12)
        fw = FakeFirmware(port=port)
        async with fw.running():
            transport = TcpTransport(host="127.0.0.1", port=port, connect_timeout=2.0)
            await transport.connect()
            adapter = FirmwareAdapter(transport, bus)
            task = asyncio.create_task(adapter.run())

            q_audio = bus.subscribe(AudioChunkIn)
            assert await fw.wait_connected(timeout=2.0)
            await asyncio.sleep(0.05)

            await fw.send_audio_chunks(count=3)
            chunks = []
            for _ in range(3):
                c = await asyncio.wait_for(q_audio.get(), timeout=2.0)
                chunks.append(c)
            assert all(isinstance(c, AudioChunkIn) for c in chunks)
            assert all(len(c.pcm) == 512 for c in chunks)

            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
            await transport.disconnect()

    async def test_status_update_received(self, bus: EventBus):
        port = _port(13)
        fw = FakeFirmware(port=port)
        async with fw.running():
            transport = TcpTransport(host="127.0.0.1", port=port, connect_timeout=2.0)
            await transport.connect()
            adapter = FirmwareAdapter(transport, bus)
            task = asyncio.create_task(adapter.run())

            q_status = bus.subscribe(StatusUpdate)
            assert await fw.wait_connected(timeout=2.0)
            await asyncio.sleep(0.05)

            await fw.send_status(state=1, valence=0.5, activation=0.3, attention=0.8, health=1)
            event = await asyncio.wait_for(q_status.get(), timeout=2.0)
            assert isinstance(event, StatusUpdate)
            assert event.state == 1
            assert abs(event.valence - 0.5) < 0.01

            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
            await transport.disconnect()


# ── Envio de comandos ao firmware ─────────────────────────────────────────────

class TestCommandSending:
    async def test_speech_cancel_sent_when_barge_in_supported(self, bus: EventBus):
        """send_speech_cancel() envia frame ao firmware se barge_in suportado."""
        port = _port(20)
        fw = FakeFirmware(port=port, firmware_features=["barge_in"])
        async with fw.running():
            transport = TcpTransport(host="127.0.0.1", port=port, connect_timeout=2.0)
            await transport.connect()
            adapter = FirmwareAdapter(transport, bus)
            task = asyncio.create_task(adapter.run())

            q_conn = bus.subscribe(FirmwareConnected)
            await asyncio.wait_for(q_conn.get(), timeout=2.0)

            await adapter.send_speech_cancel(turn_id=42)
            await asyncio.sleep(0.1)

            types = fw.received_types()
            assert MSG_SPEECH_CANCEL in types

            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
            await transport.disconnect()

    async def test_speech_cancel_not_sent_without_barge_in(self, bus: EventBus):
        """send_speech_cancel() nao envia frame se firmware nao suporta barge_in."""
        port = _port(21)
        fw = FakeFirmware(port=port, firmware_features=[])
        async with fw.running():
            transport = TcpTransport(host="127.0.0.1", port=port, connect_timeout=2.0)
            await transport.connect()
            adapter = FirmwareAdapter(transport, bus)
            task = asyncio.create_task(adapter.run())

            q_conn = bus.subscribe(FirmwareConnected)
            await asyncio.wait_for(q_conn.get(), timeout=2.0)

            await adapter.send_speech_cancel(turn_id=1)
            await asyncio.sleep(0.1)

            assert MSG_SPEECH_CANCEL not in fw.received_types()

            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
            await transport.disconnect()


# ── Desconexao e FirmwareDisconnected ─────────────────────────────────────────

class TestDisconnection:
    async def test_firmware_disconnected_on_server_close(self, bus: EventBus):
        """Quando o firmware fecha a conexao, FirmwareDisconnected chega no bus."""
        port = _port(30)
        fw = FakeFirmware(port=port)
        transport = TcpTransport(host="127.0.0.1", port=port, connect_timeout=2.0)
        q_disc = bus.subscribe(FirmwareDisconnected)

        async with fw.running():
            await transport.connect()
            adapter = FirmwareAdapter(transport, bus)
            task = asyncio.create_task(adapter.run())

            assert await fw.wait_connected(timeout=2.0)
            await fw.stop()

        try:
            event = await asyncio.wait_for(q_disc.get(), timeout=2.0)
            assert isinstance(event, FirmwareDisconnected)
        except asyncio.TimeoutError:
            pytest.fail("FirmwareDisconnected nao chegou apos servidor fechar")
        finally:
            await asyncio.gather(task, return_exceptions=True)
            await transport.disconnect()


# ── 5 conexoes/reconexoes (criterio Fase 2) ───────────────────────────────────

class TestReconnection:
    async def test_five_reconnections_no_error(self):
        """Criterio Fase 2: 5 conexoes/reconexoes seguidas sem erro."""
        port = _port(40)
        bus = EventBus(default_maxsize=512)
        reconnect = _fast_reconnect()

        connections = []
        disconnections = []
        q_conn = bus.subscribe(FirmwareConnected)
        q_disc = bus.subscribe(FirmwareDisconnected)

        supervisor = ConnectionSupervisor(
            transport_factory=_tcp_factory(port),
            bus=bus,
            reconnect=reconnect,
        )
        supervisor_task = asyncio.create_task(supervisor.run(), name="test_supervisor")

        try:
            for cycle in range(5):
                fw = FakeFirmware(port=port)
                await fw.start()

                try:
                    evt = await asyncio.wait_for(q_conn.get(), timeout=3.0)
                    connections.append(evt)
                    assert isinstance(evt, FirmwareConnected), \
                        f"ciclo {cycle}: esperava FirmwareConnected"
                except asyncio.TimeoutError:
                    pytest.fail(f"Ciclo {cycle}: timeout aguardando FirmwareConnected")

                await fw.stop()

                try:
                    disc = await asyncio.wait_for(q_disc.get(), timeout=3.0)
                    disconnections.append(disc)
                    assert isinstance(disc, FirmwareDisconnected), \
                        f"ciclo {cycle}: esperava FirmwareDisconnected"
                except asyncio.TimeoutError:
                    pytest.fail(f"Ciclo {cycle}: timeout aguardando FirmwareDisconnected")

        finally:
            await supervisor.shutdown()
            supervisor_task.cancel()
            await asyncio.gather(supervisor_task, return_exceptions=True)
            await bus.close()

        assert len(connections) == 5, f"Esperava 5 conexoes, obteve {len(connections)}"
        assert len(disconnections) == 5, f"Esperava 5 desconexoes, obteve {len(disconnections)}"

    async def test_reconnect_after_firmware_restart(self):
        """Supervisor reconecta automaticamente quando firmware reinicia."""
        port = _port(41)
        bus = EventBus(default_maxsize=256)
        reconnect = _fast_reconnect()
        q_conn = bus.subscribe(FirmwareConnected)

        supervisor = ConnectionSupervisor(
            transport_factory=_tcp_factory(port),
            bus=bus,
            reconnect=reconnect,
        )
        supervisor_task = asyncio.create_task(supervisor.run())

        try:
            fw1 = FakeFirmware(port=port)
            await fw1.start()
            evt1 = await asyncio.wait_for(q_conn.get(), timeout=3.0)
            assert isinstance(evt1, FirmwareConnected)
            await fw1.stop()

            q_disc = bus.subscribe(FirmwareDisconnected)
            await asyncio.wait_for(q_disc.get(), timeout=2.0)

            await asyncio.sleep(0.1)
            fw2 = FakeFirmware(port=port)
            await fw2.start()
            evt2 = await asyncio.wait_for(q_conn.get(), timeout=3.0)
            assert isinstance(evt2, FirmwareConnected)
            await fw2.stop()
        finally:
            await supervisor.shutdown()
            supervisor_task.cancel()
            await asyncio.gather(supervisor_task, return_exceptions=True)
            await bus.close()


# ── Integridade de frames (zero corrupcao) ────────────────────────────────────

class TestFrameIntegrity:
    async def test_100_audio_chunks_no_corruption(self, bus: EventBus):
        """100 audio chunks recebidos sem corrupcao de CRC."""
        port = _port(50)
        fw = FakeFirmware(port=port)
        async with fw.running():
            transport = TcpTransport(host="127.0.0.1", port=port, connect_timeout=2.0)
            await transport.connect()
            adapter = FirmwareAdapter(transport, bus)
            task = asyncio.create_task(adapter.run())

            # maxsize=-1 = ilimitado: necessario para 100 chunks (default seria 64)
            q_audio = bus.subscribe(AudioChunkIn, maxsize=-1)
            assert await fw.wait_connected(timeout=2.0)
            await asyncio.sleep(0.05)

            count = 100
            await fw.send_audio_chunks(count=count, samples_each=256)

            chunks = []
            for _ in range(count):
                c = await asyncio.wait_for(q_audio.get(), timeout=5.0)
                chunks.append(c)

            assert len(chunks) == count
            assert all(len(c.pcm) == 512 for c in chunks), "Chunk com tamanho incorreto"
            assert all(isinstance(c.pcm, bytes) for c in chunks)

            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
            await transport.disconnect()

    async def test_voice_sequence_start_chunks_end(self, bus: EventBus):
        """Sequencia completa: VOICE_START -> AUDIO_CHUNKS -> VOICE_END, na ordem."""
        port = _port(51)
        fw = FakeFirmware(port=port)
        async with fw.running():
            transport = TcpTransport(host="127.0.0.1", port=port, connect_timeout=2.0)
            await transport.connect()
            adapter = FirmwareAdapter(transport, bus)
            task = asyncio.create_task(adapter.run())

            q_all = bus.subscribe(
                VoiceActivityStart, AudioChunkIn, VoiceActivityEnd,
                maxsize=-1,
            )
            assert await fw.wait_connected(timeout=2.0)
            await asyncio.sleep(0.05)

            await fw.send_voice_start()
            await fw.send_audio_chunks(count=5)
            await fw.send_voice_end(reason=0)

            events = []
            for _ in range(7):  # 1 start + 5 chunks + 1 end
                e = await asyncio.wait_for(q_all.get(), timeout=3.0)
                events.append(e)

            assert isinstance(events[0], VoiceActivityStart)
            for i in range(1, 6):
                assert isinstance(events[i], AudioChunkIn), \
                    f"evento {i} deveria ser AudioChunkIn"
            assert isinstance(events[6], VoiceActivityEnd)

            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
            await transport.disconnect()
