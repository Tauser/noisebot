"""Transport factory helpers for the server runtime."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

from .base import Transport
from .tcp import TcpTransport
from .uart import UartTransport


def create_transport_factory(config: Any) -> Callable[[], Transport]:
    """Create a transport factory from a bridge/server config object.

    The current config shape is ``bridgev2.config.BridgeV2Config``. Keeping this
    helper in the server package gives us a stable seam for the later config
    migration without changing connection behavior now.
    """
    transport = config.transport
    reconnect = config.reconnect

    if transport.use_tcp:
        return lambda: TcpTransport(
            host=transport.host,
            port=transport.port,
            connect_timeout=reconnect.connect_timeout_s,
        )

    return lambda: UartTransport(
        port=transport.uart,
        baudrate=transport.baudrate,
    )


__all__ = ["create_transport_factory"]
