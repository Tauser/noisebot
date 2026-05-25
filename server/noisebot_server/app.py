"""Application facade for the local NoiseBot server."""

from __future__ import annotations

from ._compat import ensure_bridgev2_path

ensure_bridgev2_path()

from bridgev2.app import Application as BridgeApplication


class NoiseBotServer(BridgeApplication):
    """Phase-1 server application.

    The implementation is intentionally inherited from bridgev2 for now. Later
    phases will move transport, ops, agent and vision responsibilities behind
    explicit server-owned interfaces.
    """
