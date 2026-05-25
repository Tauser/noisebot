"""Firmware transport and protocol internals.

Phase 2 owns the server-side boundary while delegating implementation to
``bridge_v2``. This lets new server code depend on ``noisebot_server`` imports
instead of reaching into bridge internals directly.
"""

from __future__ import annotations

from .adapter import FirmwareAdapter
from .base import Transport
from .factory import create_transport_factory
from .supervisor import ConnectionSupervisor
from .tcp import TcpTransport
from .uart import UartTransport

__all__ = [
    "ConnectionSupervisor",
    "FirmwareAdapter",
    "TcpTransport",
    "Transport",
    "UartTransport",
    "create_transport_factory",
]
