"""Fake firmware facade for server debug tools."""

from __future__ import annotations

from ..._compat import ensure_bridgev2_path

ensure_bridgev2_path()

from bridgev2.debug.fake_firmware import (
    CHUNK_SAMPLES,
    FIRMWARE_HELLO,
    SAMPLE_RATE,
    FakeFirmware,
    ReceivedFrame,
)

__all__ = [
    "CHUNK_SAMPLES",
    "FIRMWARE_HELLO",
    "FakeFirmware",
    "ReceivedFrame",
    "SAMPLE_RATE",
]
