"""bridgev2.__main__ — Entrypoint: monta o grafo e roda o event loop.

Uso:
    python -m bridgev2                          # inicia normalmente
    python -m bridgev2 --host 192.168.1.10
    python -m bridgev2 --dry-run
    python -m bridgev2 --log-file ~/.bridgev2/logs/bridge.log
    python -m bridgev2 service install          # instala como serviço do SO
    python -m bridgev2 service uninstall
    python -m bridgev2 service status
    python -m bridgev2 service start
    python -m bridgev2 service stop
    python -m bridgev2 debug transcript "olá"
    python -m bridgev2 debug fake-fw
    python -m bridgev2 --help
"""
from __future__ import annotations

import argparse
import asyncio
import logging
import logging.handlers
import signal
import sys
from pathlib import Path

from .app import Application
from .config import find_env_file, load_config, PipelineMode, LlmProvider

_DEFAULT_LOG_FILE = Path.home() / ".bridgev2" / "logs" / "bridge.log"
_LOG_MAX_BYTES    = 5 * 1024 * 1024   # 5 MB por arquivo
_LOG_BACKUP_COUNT = 3                  # mantém bridge.log + bridge.log.1-3


def _parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="bridgev2",
        description="NoiseBot Bridge v2 — pipeline de voz async de baixa latência",
    )
    sub = p.add_subparsers(dest="command")

    # -- Subcomando: service --------------------------------------------------
    svc = sub.add_parser("service", help="Gerenciar como serviço do sistema operacional")
    svc_sub = svc.add_subparsers(dest="service_command", required=True)
    svc_sub.add_parser("install",   help="Instala o bridge como serviço (Task Scheduler / systemd)")
    svc_sub.add_parser("uninstall", help="Remove o serviço")
    svc_sub.add_parser("status",    help="Exibe o status do serviço")
    svc_sub.add_parser("start",     help="Inicia o serviço")
    svc_sub.add_parser("stop",      help="Para o serviço")

    # -- Subcomando: debug ----------------------------------------------------
    debug = sub.add_parser("debug", help="Ferramentas manuais de teste")
    debug_sub = debug.add_subparsers(dest="debug_command", required=True)
    transcript = debug_sub.add_parser("transcript", help="Injeta FinalTranscript sintético")
    transcript.add_argument("text", help="Texto a entregar ao orchestrator")
    transcript.add_argument("--turn-id", type=int, default=1)

    fake_fw = debug_sub.add_parser("fake-fw", help="Sobe o simulador de firmware TCP")
    fake_fw.add_argument("--host", default="127.0.0.1")
    fake_fw.add_argument("--port", type=int, default=9001)
    fake_fw.add_argument(
        "--features",
        default="",
        help="Features anunciadas, separadas por vírgula. Ex: turn_id,barge_in",
    )
    fake_fw.add_argument(
        "--auto-silence-chunks",
        type=int,
        default=0,
        help="Após conectar, injeta sessão de voz com N chunks silenciosos.",
    )

    # -- Flags globais --------------------------------------------------------
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
        "--llm",
        choices=[p.value for p in LlmProvider],
        help="Provider LLM (sobrescreve NOISEBOT_LLM_PROVIDER)",
    )
    p.add_argument(
        "--model",
        help="Modelo LLM (sobrescreve NOISEBOT_LLM_MODEL)",
    )
    p.add_argument(
        "--log-level",
        default=None,
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        help="Nível de log (sobrescreve NOISEBOT_LOG_LEVEL)",
    )
    p.add_argument("--env", help="Caminho para arquivo .env alternativo")
    p.add_argument(
        "--log-file",
        default=None,
        metavar="PATH",
        help=(
            f"Arquivo de log com rotação automática (padrão: {_DEFAULT_LOG_FILE}). "
            "Use 'stderr' para desabilitar arquivo."
        ),
    )
    return p.parse_args()


def _setup_logging(level: str, log_file: str | None) -> None:
    """Configura logging: stderr sempre + arquivo rotativo opcional."""
    fmt = "%(asctime)s %(levelname)s %(name)s: %(message)s"
    root = logging.getLogger()
    root.setLevel(level)

    # Handler stderr
    sh = logging.StreamHandler(sys.stderr)
    sh.setFormatter(logging.Formatter(fmt))
    root.addHandler(sh)

    # Handler de arquivo rotativo
    if log_file and log_file.lower() != "stderr":
        path = Path(log_file).expanduser()
    else:
        path = _DEFAULT_LOG_FILE

    if log_file != "stderr":
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            fh = logging.handlers.RotatingFileHandler(
                path,
                maxBytes=_LOG_MAX_BYTES,
                backupCount=_LOG_BACKUP_COUNT,
                encoding="utf-8",
            )
            fh.setFormatter(logging.Formatter(fmt))
            root.addHandler(fh)
            logging.getLogger("bridgev2").debug("Log em arquivo: %s", path)
        except OSError as exc:
            logging.getLogger("bridgev2").warning(
                "Não foi possível abrir arquivo de log %s: %s", path, exc
            )


def main() -> None:
    args = _parse_args()

    # -- Subcomando service ---------------------------------------------------
    if args.command == "service":
        logging.basicConfig(
            level=logging.INFO,
            format="%(asctime)s %(levelname)s %(name)s: %(message)s",
            stream=sys.stderr,
        )
        from .service.manager import get_manager
        mgr = get_manager()
        try:
            if args.service_command == "install":
                mgr.install()
            elif args.service_command == "uninstall":
                mgr.uninstall()
            elif args.service_command == "status":
                print(mgr.status())
            elif args.service_command == "start":
                mgr.start()
                print("Serviço iniciado.")
            elif args.service_command == "stop":
                mgr.stop()
                print("Serviço parado.")
        except Exception as exc:
            print(f"Erro: {exc}", file=sys.stderr)
            raise SystemExit(1)
        return

    # -- Subcomando debug -----------------------------------------------------
    if args.command == "debug":
        logging.basicConfig(
            level=logging.INFO,
            format="%(asctime)s %(levelname)s %(name)s: %(message)s",
            stream=sys.stderr,
        )
        from .debug.manual import run_fake_firmware_debug, run_transcript_debug

        if args.debug_command == "transcript":
            raise SystemExit(asyncio.run(run_transcript_debug(args.text, args.turn_id)))
        if args.debug_command == "fake-fw":
            features = [f.strip() for f in args.features.split(",") if f.strip()]
            raise SystemExit(asyncio.run(run_fake_firmware_debug(
                args.host,
                args.port,
                features,
                args.auto_silence_chunks,
            )))

    # -- Modo normal: executa o bridge ----------------------------------------
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
    if args.llm:
        os.environ["NOISEBOT_LLM_PROVIDER"] = args.llm
    if args.model:
        os.environ["NOISEBOT_LLM_MODEL"] = args.model
    if args.log_level:
        os.environ["NOISEBOT_LOG_LEVEL"] = args.log_level

    env_file = find_env_file(args.env)
    config = load_config(env_file)

    _setup_logging(config.log_level.value, getattr(args, "log_file", None))
    log = logging.getLogger("bridgev2")
    log.info("Bridge v2 iniciando. pipeline_mode=%s", config.pipeline_mode.value)
    if env_file is not None:
        log.info("Config: .env carregado de %s", env_file)
    else:
        log.warning("Config: nenhum .env encontrado")
    log.info("Config: %s", config.safe_dict())

    app = Application(config)

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
