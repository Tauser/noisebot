"""Testes de bridgev2.runtime.bus — Fase 1 critério: lint + testes passam."""
from __future__ import annotations

import asyncio
import pytest

from bridgev2.runtime.bus import EventBus
from bridgev2.runtime.events import (
    VoiceActivityStart,
    VoiceActivityEnd,
    AudioChunkIn,
    FirmwareConnected,
    ShutdownRequested,
)


class TestEventBusCorePublish:
    async def test_publish_received_by_subscriber(self, bus: EventBus):
        q = bus.subscribe()
        await bus.publish(VoiceActivityStart())
        event = q.get_nowait()
        assert isinstance(event, VoiceActivityStart)

    async def test_multiple_subscribers_all_receive(self, bus: EventBus):
        q1 = bus.subscribe()
        q2 = bus.subscribe()
        await bus.publish(VoiceActivityStart())
        assert isinstance(q1.get_nowait(), VoiceActivityStart)
        assert isinstance(q2.get_nowait(), VoiceActivityStart)

    async def test_filtered_subscriber_receives_only_matching(self, bus: EventBus):
        q_voice = bus.subscribe(VoiceActivityStart)
        q_audio = bus.subscribe(AudioChunkIn)

        await bus.publish(VoiceActivityStart())
        await bus.publish(AudioChunkIn(pcm=b"\x00" * 512))

        assert isinstance(q_voice.get_nowait(), VoiceActivityStart)
        assert q_voice.empty()  # AudioChunkIn não chegou aqui

        assert isinstance(q_audio.get_nowait(), AudioChunkIn)
        assert q_audio.empty()  # VoiceActivityStart não chegou aqui

    async def test_no_subscribers_publish_ok(self, bus: EventBus):
        # Publicar sem assinantes não deve lançar exceção
        await bus.publish(VoiceActivityStart())

    async def test_publish_nowait_sync(self, bus: EventBus):
        q = bus.subscribe()
        bus.publish_nowait(FirmwareConnected())
        assert isinstance(q.get_nowait(), FirmwareConnected)


class TestEventBusFilter:
    async def test_subscribe_multiple_types(self, bus: EventBus):
        q = bus.subscribe(VoiceActivityStart, VoiceActivityEnd)
        await bus.publish(VoiceActivityStart())
        await bus.publish(VoiceActivityEnd())
        await bus.publish(AudioChunkIn(pcm=b"\x00" * 512))  # filtrado

        e1 = q.get_nowait()
        e2 = q.get_nowait()
        assert isinstance(e1, VoiceActivityStart)
        assert isinstance(e2, VoiceActivityEnd)
        assert q.empty()

    async def test_unsubscribe(self, bus: EventBus):
        q = bus.subscribe()
        bus.unsubscribe(q)
        await bus.publish(VoiceActivityStart())
        assert q.empty()


class TestEventBusBackpressure:
    async def test_full_queue_does_not_block(self, bus: EventBus):
        """Fila cheia deve descartar silenciosamente, nunca bloquear o loop."""
        q = bus.subscribe(maxsize=2)
        await bus.publish(VoiceActivityStart())
        await bus.publish(VoiceActivityStart())
        # Terceiro evento — fila cheia — não deve bloquear
        await bus.publish(VoiceActivityStart())  # descartado
        assert q.qsize() == 2

    async def test_unlimited_queue(self, bus: EventBus):
        q = bus.subscribe(maxsize=-1)
        for _ in range(500):
            await bus.publish(AudioChunkIn(pcm=b"\x00" * 512))
        assert q.qsize() == 500


class TestEventBusClose:
    async def test_close_stops_iter_queue(self, bus: EventBus):
        q = bus.subscribe()
        received = []

        async def _consumer():
            async for event in EventBus.iter_queue(q):
                received.append(event)

        consumer_task = asyncio.create_task(_consumer())
        await bus.publish(VoiceActivityStart())
        await bus.close()
        await consumer_task

        assert len(received) == 1
        assert isinstance(received[0], VoiceActivityStart)

    async def test_closed_bus_publish_noop(self, bus: EventBus):
        """Após close(), publish() não deve lançar e não deve entregar eventos."""
        # Faz subscribe, publica um evento, fecha e drena — tudo numa rodada limpa
        q = bus.subscribe()

        async def _drain():
            received = []
            async for ev in EventBus.iter_queue(q):
                received.append(ev)
            return received

        drain_task = asyncio.create_task(_drain())

        await bus.publish(VoiceActivityStart())  # deve chegar
        await bus.close()                         # fecha e envia sentinel
        received = await drain_task               # drena (inclusive o sentinel)

        # Após close, publish é noop — sem erros e sem entregas adicionais
        await bus.publish(VoiceActivityStart())
        await bus.publish(VoiceActivityStart())

        assert len(received) == 1
        assert isinstance(received[0], VoiceActivityStart)
        assert bus._closed  # bus permanece fechado
        assert bus.subscriber_count == 0

    async def test_subscriber_count(self, bus: EventBus):
        assert bus.subscriber_count == 0
        bus.subscribe()
        bus.subscribe()
        assert bus.subscriber_count == 2


class TestEventBusIterQueue:
    async def test_iter_queue_yields_events(self, bus: EventBus):
        q = bus.subscribe()
        await bus.publish(VoiceActivityStart())
        await bus.publish(FirmwareConnected())
        await bus.close()

        events = []
        async for e in EventBus.iter_queue(q):
            events.append(e)

        assert len(events) == 2
        assert isinstance(events[0], VoiceActivityStart)
        assert isinstance(events[1], FirmwareConnected)


class TestEventDataclasses:
    def test_voice_activity_start_has_timestamp(self):
        e = VoiceActivityStart()
        assert e.t > 0

    def test_audio_chunk_in(self):
        pcm = b"\x00\x01" * 256  # 512 bytes = 256 amostras int16
        e = AudioChunkIn(pcm=pcm, seq=42)
        assert len(e.pcm) == 512
        assert e.seq == 42

    def test_firmware_connected_defaults(self):
        e = FirmwareConnected()
        assert e.peer_capabilities == {}

    def test_shutdown_requested(self):
        e = ShutdownRequested(reason="test")
        assert e.reason == "test"
