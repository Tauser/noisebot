"""Fixtures compartilhadas para os testes do bridge v2."""
from __future__ import annotations
import pytest
from aiohttp.test_utils import TestClient, TestServer
from bridgev2.runtime.bus import EventBus


@pytest.fixture()
def bus() -> EventBus:
    return EventBus(default_maxsize=64)


@pytest.fixture()
async def aiohttp_client():
    """Fixture leve compatível com pytest-aiohttp para testar aiohttp.web.Application."""
    clients: list[TestClient] = []

    async def _make_client(app):
        server = TestServer(app)
        client = TestClient(server)
        await client.start_server()
        clients.append(client)
        return client

    try:
        yield _make_client
    finally:
        for client in clients:
            await client.close()
