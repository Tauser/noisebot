"""bridgev2.__main__ — Entrypoint: monta o grafo e roda o event loop.

Uso:
    python -m bridgev2
    python -m bridgev2 --host 192.168.1.10
    python -m bridgev2 --dry-run
    python -m bridgev2 --help
"""
from __future__ import annotations

import argparse
import asyncio
import logging
import signal
import sys

from .app import Application
from .config import load_config, PipelineMode


def _parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="bridgev2",
        description="NoiseBot Bridge v2 — pipeline de voz async de baixa latência",
    )
    p.add_argument("--host", help="IP do ESP32 (sobrescreve NOISEBOT_HOST)")
    p.add_argument("--port", type=int, help="Porta TCP (padrão: 9000)")
    p.add_argument("--uart", help="Porta UART (sobrescreve NOISEBOT_UART)")
    p.add_argument("--dry-run", action="store_true", help="Sem conexão real (dev)")
    p.add_argument(
        "--pipeline",
        choices=[m.value for m in PipelineMode],
        help="Modo do pipeline (sobrescreve NOISEBOT_PIPELINE_MODE)",
    )
    p.add_argument(
        "--log-level",
        default=None,
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        help="Nível de log (sobrescreve NOISEBOT_LOG_LEVEL)",
    )
    p.add_argument("--env", help="Caminho para arquivo .env alternativo")
    return p.parse_args()


def main() -> None:
    args = _parse_args()

    # Aplica overrides de CLI em variáveis de ambiente antes de load_config
    import os
    if args.host:
        os.environ["NOISEBOT_HOST"] = args.host
    if args.port:
        os.environ["NOISEBOT_PORT"] = str(args.port)
    if args.uart:
        os.environ["NOISEBOT_UART"] = args.uart
    if args.dry_run:
        os.environ["NOISEBOT_DRY_RUN"] = "true"
    if args.pipeline:
        os.environ["NOISEBOT_PIPELINE_MODE"] = args.pipeline
    if args.log_level:
        os.environ["NOISEBOT_LOG_LEVEL"] = args.log_level

    config = load_config(args.env)

    # Configura logging
    logging.basicConfig(
        level=config.log_level.value,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        stream=sys.stderr,
    )
    log = logging.getLogger("bridgev2")
    log.info("Bridge v2 iniciando. pipeline_mode=%s", config.pipeline_mode.value)
    log.info("Config: %s", config.safe_dict())

    app = Application(config)

    # Registra SIGINT/SIGTERM para shutdown gracioso
    loop = asyncio.new_event_loop()

    def _shutdown(sig_name: str) -> None:
        log.info("Sinal %s recebido — encerrando...", sig_name)
        loop.create_task(app.shutdown())

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _shutdown, sig.name)
        except NotImplementedError:
            # Windows não suporta add_signal_handler para todos os sinais
            pass

    try:
        loop.run_until_complete(app.run())
    except KeyboardInterrupt:
        log.info("Interrompido pelo teclado")
    finally:
        loop.run_until_complete(app.shutdown())
        loop.close()
        log.info("Bridge v2 encerrado.")


if __name__ == "__main__":
    main()
