"""bridgev2.metrics.logfmt — Logs estruturados JSON lines / logfmt (Fase 4)."""
from __future__ import annotations
import json
import logging


def structured(logger: logging.Logger, level: int, event: str, **fields) -> None:
    """Emite log estruturado como JSON line."""
    record = {"event": event, **fields}
    # Remove campos None
    record = {k: v for k, v in record.items() if v is not None}
    logger.log(level, json.dumps(record, ensure_ascii=False, default=str))
