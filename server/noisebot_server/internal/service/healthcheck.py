"""Healthcheck facade for the server runtime."""

from __future__ import annotations

from ..._compat import ensure_bridgev2_path

ensure_bridgev2_path()

from bridgev2.service.healthcheck import (
    HEALTHCHECK_INTERVAL_S,
    HEALTHCHECK_MAX_AGE_S,
    healthcheck_loop,
    is_healthy,
    remove_healthcheck,
    write_healthy,
    write_unhealthy,
)

__all__ = [
    "HEALTHCHECK_INTERVAL_S",
    "HEALTHCHECK_MAX_AGE_S",
    "healthcheck_loop",
    "is_healthy",
    "remove_healthcheck",
    "write_healthy",
    "write_unhealthy",
]
