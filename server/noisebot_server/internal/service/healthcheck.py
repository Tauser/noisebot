"""Server healthcheck file and heartbeat loop."""

from __future__ import annotations

import asyncio
import logging
import tempfile
import time
from pathlib import Path

log = logging.getLogger(__name__)

HEALTHCHECK_FILE = Path(tempfile.gettempdir()) / "noisebot-server.health"
HEALTHCHECK_INTERVAL_S = 10.0
HEALTHCHECK_MAX_AGE_S = 30.0


def write_healthy(extra: str = "") -> None:
    """Write the current heartbeat timestamp."""
    try:
        content = str(time.time())
        if extra:
            content += f"\n{extra}"
        HEALTHCHECK_FILE.write_text(content, encoding="utf-8")
    except OSError as exc:
        log.warning("healthcheck: nao foi possivel escrever %s: %s", HEALTHCHECK_FILE, exc)


def write_unhealthy(reason: str = "unknown") -> None:
    """Mark the server as unhealthy for diagnostics."""
    try:
        HEALTHCHECK_FILE.write_text(f"0\nUNHEALTHY: {reason}", encoding="utf-8")
    except OSError:
        pass


def is_healthy(max_age_s: float = HEALTHCHECK_MAX_AGE_S) -> bool:
    """Return True if the heartbeat file is recent."""
    if not HEALTHCHECK_FILE.exists():
        return False
    try:
        first_line = HEALTHCHECK_FILE.read_text(encoding="utf-8").splitlines()[0]
        timestamp = float(first_line)
        return timestamp > 0 and (time.time() - timestamp) < max_age_s
    except (IndexError, OSError, ValueError):
        return False


def remove_healthcheck() -> None:
    """Remove the healthcheck file on shutdown."""
    try:
        HEALTHCHECK_FILE.unlink(missing_ok=True)
    except OSError:
        pass


async def healthcheck_loop(interval_s: float = HEALTHCHECK_INTERVAL_S) -> None:
    """Keep the heartbeat file fresh until the task is cancelled."""
    try:
        log.debug(
            "Healthcheck loop iniciado interval=%.0fs file=%s",
            interval_s,
            HEALTHCHECK_FILE,
        )
        while True:
            write_healthy()
            await asyncio.sleep(interval_s)
    except asyncio.CancelledError:
        remove_healthcheck()
        log.debug("Healthcheck loop encerrado.")
