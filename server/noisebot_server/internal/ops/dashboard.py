"""Ops dashboard facade."""

from __future__ import annotations

from ..._compat import ensure_bridgev2_path

ensure_bridgev2_path()

from bridgev2.ops.dashboard import get_dashboard_html

__all__ = ["get_dashboard_html"]
