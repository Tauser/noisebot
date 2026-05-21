"""Fixtures compartilhadas para os testes do bridge v2."""
from __future__ import annotations
import pytest
from bridgev2.runtime.bus import EventBus


@pytest.fixture()
def bus() -> EventBus:
    return EventBus(default_maxsize=64)
