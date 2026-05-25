"""NoiseBot server command-line interface."""

from __future__ import annotations

import argparse
import asyncio
import os
from collections.abc import Sequence

from ._compat import ensure_bridgev2_path
from .config import LlmProvider, PipelineMode, find_env_file, load_config
from .runtime import DEFAULT_LOG_FILE, run_server


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="noisebot-server",
        description="NoiseBot local companion server",
    )
    sub = parser.add_subparsers(dest="command")

    service = sub.add_parser("service", help="Gerenciar como servico do sistema")
    service_sub = service.add_subparsers(dest="service_command", required=True)
    service_sub.add_parser("install")
    service_sub.add_parser("uninstall")
    service_sub.add_parser("status")
    service_sub.add_parser("start")
    service_sub.add_parser("stop")

    debug = sub.add_parser("debug", help="Ferramentas manuais de teste")
    debug_sub = debug.add_subparsers(dest="debug_command", required=True)
    transcript = debug_sub.add_parser("transcript")
    transcript.add_argument("text")
    transcript.add_argument("--turn-id", type=int, default=1)

    fake_fw = debug_sub.add_parser("fake-fw")
    fake_fw.add_argument("--host", default="127.0.0.1")
    fake_fw.add_argument("--port", type=int, default=9001)
    fake_fw.add_argument("--features", default="")
    fake_fw.add_argument("--auto-silence-chunks", type=int, default=0)

    parser.add_argument("--host", help="IP do ESP32")
    parser.add_argument("--port", type=int, help="Porta TCP")
    parser.add_argument("--uart", help="Porta UART")
    parser.add_argument("--dry-run", action="store_true", help="Sem conexao real")
    parser.add_argument(
        "--pipeline",
        choices=[mode.value for mode in PipelineMode],
        help="Modo do pipeline",
    )
    parser.add_argument(
        "--llm",
        choices=[provider.value for provider in LlmProvider],
        help="Provider LLM",
    )
    parser.add_argument("--model", help="Modelo LLM")
    parser.add_argument(
        "--log-level",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        help="Nivel de log",
    )
    parser.add_argument("--env", help="Caminho para arquivo .env alternativo")
    parser.add_argument(
        "--log-file",
        default=None,
        metavar="PATH",
        help=(
            f"Arquivo de log com rotacao automatica (padrao: {DEFAULT_LOG_FILE}). "
            "Use 'stderr' para desabilitar arquivo."
        ),
    )
    return parser.parse_args(argv)


def apply_env_overrides(args: argparse.Namespace) -> None:
    """Apply CLI flags to environment before loading config."""
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


def run_debug_command(args: argparse.Namespace) -> None:
    """Run server-owned debug commands."""
    from .internal.debug.manual import run_fake_firmware_debug, run_transcript_debug

    if args.debug_command == "transcript":
        raise SystemExit(asyncio.run(run_transcript_debug(args.text, args.turn_id)))
    if args.debug_command == "fake-fw":
        features = [item.strip() for item in args.features.split(",") if item.strip()]
        raise SystemExit(
            asyncio.run(
                run_fake_firmware_debug(
                    args.host,
                    args.port,
                    features,
                    args.auto_silence_chunks,
                )
            )
        )
    raise SystemExit(2)


def main(argv: Sequence[str] | None = None) -> None:
    args = parse_args(argv)

    if args.command == "service":
        ensure_bridgev2_path()
        from bridgev2.__main__ import main as bridge_main

        bridge_main()
        return
    if args.command == "debug":
        run_debug_command(args)
        return

    apply_env_overrides(args)
    env_file = find_env_file(args.env)
    config = load_config(env_file)
    run_server(config, log_file=args.log_file)
