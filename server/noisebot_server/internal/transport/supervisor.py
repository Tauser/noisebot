"""Connection supervisor facade."""

from __future__ import annotations

from ..._compat import ensure_bridgev2_path

ensure_bridgev2_path()

from bridgev2.transport.reconnect import ConnectionSupervisor

__all__ = ["ConnectionSupervisor"]
