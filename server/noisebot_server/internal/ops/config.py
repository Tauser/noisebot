"""Runtime configuration controller facade."""

from __future__ import annotations

from ..._compat import ensure_bridgev2_path

ensure_bridgev2_path()

from bridgev2.ops.config_controller import (
    MUTABLE_FIELDS,
    PROVIDER_CATALOG,
    VALID_MODES,
    ConfigController,
)

__all__ = [
    "ConfigController",
    "MUTABLE_FIELDS",
    "PROVIDER_CATALOG",
    "VALID_MODES",
]
