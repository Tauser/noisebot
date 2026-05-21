"""bridgev2.service.healthcheck — Healthcheck (arquivo + endpoint HTTP, Fase 9)."""
from __future__ import annotations
import pathlib
import time

HEALTHCHECK_FILE = pathlib.Path("/tmp/bridgev2.health")


def write_healthy() -> None:
    """Atualiza arquivo de health com timestamp atual."""
    HEALTHCHECK_FILE.write_text(str(time.time()))


def is_healthy(max_age_s: float = 30.0) -> bool:
    """Retorna True se o arquivo de health foi atualizado recentemente."""
    if not HEALTHCHECK_FILE.exists():
        return False
    try:
        ts = float(HEALTHCHECK_FILE.read_text())
        return (time.time() - ts) < max_age_s
    except (ValueError, OSError):
        return False
