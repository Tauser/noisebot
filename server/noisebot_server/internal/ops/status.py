"""Runtime status store facade."""

from __future__ import annotations

from ..._compat import ensure_bridgev2_path

ensure_bridgev2_path()

from bridgev2.ops.status_store import ErrorEntry, StatusStore

__all__ = ["ErrorEntry", "StatusStore"]
