"""Transport interface facade."""

from __future__ import annotations

from ..._compat import ensure_bridgev2_path

ensure_bridgev2_path()

from bridgev2.transport.base import Transport

__all__ = ["Transport"]
