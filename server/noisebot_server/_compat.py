"""Compatibility helpers while server code is migrated from bridge_v2."""

from __future__ import annotations

import sys
from pathlib import Path


def ensure_bridgev2_path() -> None:
    """Make the existing bridge_v2 package importable from the server tree.

    This is intentionally small and temporary. During migration, server modules
    can wrap existing bridgev2 modules without duplicating logic or changing the
    current runtime behavior.
    """
    repo_root = Path(__file__).resolve().parents[2].parent
    bridge_v2_path = repo_root / "bridge_v2"
    bridge_v2_str = str(bridge_v2_path)
    if bridge_v2_path.exists() and bridge_v2_str not in sys.path:
        sys.path.insert(0, bridge_v2_str)
