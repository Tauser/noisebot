"""NoiseBot server runtime shell.

This is the first server-owned composition point. The concrete implementation
still uses bridge-backed classes through server boundaries, but startup no
longer has to delegate normal execution to ``bridgev2.__main__``.
"""

from __future__ import annotations

import asyncio
import logging
import logging.handlers
import signal
import sys
from collections.abc import Callable
from pathlib import Path
from typing import Any

from .app import NoiseBotServer
from .config import BridgeV2Config

DEFAULT_LOG_FILE = Path.home() / ".noisebot" / "logs" / "server.log"
LOG_MAX_BYTES = 5 * 1024 * 1024
LOG_BACKUP_COUNT = 3


def _has_stderr_handler() -> bool:
    for handler in logging.getLogger().handlers:
        if isinstance(handler, logging.StreamHandler) and handler.stream is sys.stderr:
            return True
    return False


def setup_logging(level: str, log_file: str | None = None) -> None:
    """Configure stderr and optional rotating file logging."""
    fmt = "%(asctime)s %(levelname)s %(name)s: %(message)s"
    root = logging.getLogger()
    root.setLevel(level)

    if not _has_stderr_handler():
        stream_handler = logging.StreamHandler(sys.stderr)
        stream_handler.setFormatter(logging.Formatter(fmt))
        root.addHandler(stream_handler)

    if log_file == "stderr":
        return

    path = Path(log_file).expanduser() if log_file else DEFAULT_LOG_FILE
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        file_handler = logging.handlers.RotatingFileHandler(
            path,
            maxBytes=LOG_MAX_BYTES,
            backupCount=LOG_BACKUP_COUNT,
            encoding="utf-8",
        )
        file_handler.setFormatter(logging.Formatter(fmt))
        root.addHandler(file_handler)
        logging.getLogger("noisebot_server").debug("Log em arquivo: %s", path)
    except OSError as exc:
        logging.getLogger("noisebot_server").warning(
            "Nao foi possivel abrir arquivo de log %s: %s", path, exc
        )


def run_server(
    config: BridgeV2Config,
    *,
    log_file: str | None = None,
    app_factory: Callable[[BridgeV2Config], Any] = NoiseBotServer,
) -> None:
    """Run the server application until shutdown."""
    setup_logging(config.log_level.value, log_file)
    log = logging.getLogger("noisebot_server")
    log.info("NoiseBot server iniciando. pipeline_mode=%s", config.pipeline_mode.value)
    log.info("Config: %s", config.safe_dict())

    app = app_factory(config)
    loop = asyncio.new_event_loop()

    def _shutdown(sig_name: str) -> None:
        log.info("Sinal %s recebido - encerrando...", sig_name)
        loop.create_task(app.shutdown())

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _shutdown, sig.name)
        except NotImplementedError:
            pass

    try:
        loop.run_until_complete(app.run())
    except KeyboardInterrupt:
        log.info("Interrompido pelo teclado")
    finally:
        loop.run_until_complete(app.shutdown())
        loop.close()
        log.info("NoiseBot server encerrado.")
