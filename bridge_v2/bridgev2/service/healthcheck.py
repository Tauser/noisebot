"""bridgev2.service.healthcheck — Healthcheck de arquivo + loop assíncrono (Fase 9).

O healthcheck é um arquivo de timestamp atualizado periodicamente enquanto o
bridge está rodando. O supervisor de serviço (Task Scheduler / systemd) pode
verificar esse arquivo para decidir se o processo está vivo.

Path multiplataforma: tempdir do SO (C:\\Users\\...\\AppData\\Local\\Temp no Windows,
/tmp no Linux/Pi).
"""
from __future__ import annotations

import asyncio
import logging
import pathlib
import tempfile
import time

log = logging.getLogger(__name__)

HEALTHCHECK_FILE = pathlib.Path(tempfile.gettempdir()) / "bridgev2.health"
HEALTHCHECK_INTERVAL_S = 10.0   # atualiza a cada 10 s
HEALTHCHECK_MAX_AGE_S  = 30.0   # considerado morto após 30 s sem atualização


def write_healthy(extra: str = "") -> None:
    """Escreve timestamp atual no arquivo de healthcheck."""
    try:
        content = str(time.time())
        if extra:
            content += f"\n{extra}"
        HEALTHCHECK_FILE.write_text(content, encoding="utf-8")
    except OSError as exc:
        log.warning("healthcheck: não foi possível escrever %s: %s", HEALTHCHECK_FILE, exc)


def write_unhealthy(reason: str = "unknown") -> None:
    """Marca o serviço como não-saudável (para diagnóstico)."""
    try:
        HEALTHCHECK_FILE.write_text(f"0\nUNHEALTHY: {reason}", encoding="utf-8")
    except OSError:
        pass


def is_healthy(max_age_s: float = HEALTHCHECK_MAX_AGE_S) -> bool:
    """Retorna True se o arquivo de health foi atualizado recentemente."""
    if not HEALTHCHECK_FILE.exists():
        return False
    try:
        first_line = HEALTHCHECK_FILE.read_text(encoding="utf-8").splitlines()[0]
        ts = float(first_line)
        return ts > 0 and (time.time() - ts) < max_age_s
    except (ValueError, IndexError, OSError):
        return False


def remove_healthcheck() -> None:
    """Remove o arquivo de healthcheck ao encerrar."""
    try:
        HEALTHCHECK_FILE.unlink(missing_ok=True)
    except OSError:
        pass


async def healthcheck_loop(interval_s: float = HEALTHCHECK_INTERVAL_S) -> None:
    """Coroutine assíncrona que atualiza o healthcheck periodicamente.

    Deve rodar como asyncio.Task dentro do Application.run().
    Cancelável — ao cancelar, remove o arquivo para sinalizar parada.
    """
    try:
        log.debug("Healthcheck loop iniciado (intervalo=%.0f s, arquivo=%s)",
                  interval_s, HEALTHCHECK_FILE)
        while True:
            write_healthy()
            await asyncio.sleep(interval_s)
    except asyncio.CancelledError:
        remove_healthcheck()
        log.debug("Healthcheck loop encerrado.")
