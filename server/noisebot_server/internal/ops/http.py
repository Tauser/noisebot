"""Ops HTTP server facade."""

from __future__ import annotations

from ..._compat import ensure_bridgev2_path

ensure_bridgev2_path()

from bridgev2.ops.http_api import OpsHttpServer

__all__ = ["OpsHttpServer"]
