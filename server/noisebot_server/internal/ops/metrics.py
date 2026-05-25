"""Metrics API facade."""

from __future__ import annotations

from ..._compat import ensure_bridgev2_path

ensure_bridgev2_path()

from bridgev2.ops.metrics_api import MetricsApi

__all__ = ["MetricsApi"]
