"""Local intent provider facade."""

from __future__ import annotations

from ..._compat import ensure_bridgev2_path

ensure_bridgev2_path()

from bridgev2.llm.local_intent import LocalIntentProvider

__all__ = ["LocalIntentProvider"]
