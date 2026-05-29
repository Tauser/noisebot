"""NoiseBot server command-line interface."""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
from collections.abc import Sequence

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
    fake_fw.add_argument("--audio-format", choices=["pcm16", "opus"], default="pcm16")
    fake_fw.add_argument("--auto-silence-chunks", type=int, default=0)

    audio_report = debug_sub.add_parser("audio-report")
    audio_report.add_argument("path", help="Pasta ou WAV para analisar")
    audio_report.add_argument("--output", help="Arquivo Markdown de saida")
    audio_report.add_argument("--json", action="store_true", help="Emitir JSON")

    afe_ab = debug_sub.add_parser("afe-ab")
    afe_ab.add_argument("phrase", help="Frase a repetir em RAW e AFE")
    afe_ab.add_argument("--server-url", default="http://127.0.0.1:8765")
    afe_ab.add_argument("--firmware-url", default="")
    afe_ab.add_argument("--repeat", type=int, default=1)
    afe_ab.add_argument("--timeout-s", type=float, default=35.0)
    afe_ab.add_argument("--output", help="Arquivo Markdown de saida")
    afe_ab.add_argument("--json", action="store_true", help="Emitir JSON")

    opus_test = debug_sub.add_parser("opus-selftest")
    opus_test.add_argument("--seconds", type=float, default=1.0)
    opus_test.add_argument("--bitrate", type=int, default=24000)
    opus_test.add_argument("--json", action="store_true", help="Emitir JSON")

    opus_live = debug_sub.add_parser("opus-live")
    opus_live.add_argument("phrase", nargs="?", default="fale algo curto")
    opus_live.add_argument("--server-url", default="http://127.0.0.1:8765")
    opus_live.add_argument("--firmware-url", default="")
    opus_live.add_argument("--timeout-s", type=float, default=45.0)
    opus_live.add_argument("--output", help="Arquivo Markdown/JSON de saida")
    opus_live.add_argument("--json", action="store_true", help="Emitir JSON")

    barge_live = debug_sub.add_parser("barge-live")
    barge_live.add_argument("phrase", nargs="?", default="me conte uma historia longa")
    barge_live.add_argument("--server-url", default="http://127.0.0.1:8765")
    barge_live.add_argument("--timeout-s", type=float, default=45.0)
    barge_live.add_argument("--output", help="Arquivo Markdown/JSON de saida")
    barge_live.add_argument("--json", action="store_true", help="Emitir JSON")

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
                    args.audio_format,
                    args.auto_silence_chunks,
                )
            )
        )
    if args.debug_command == "audio-report":
        from .internal.ops.audio_analysis import (
            analyze_audio_samples,
            format_audio_samples_markdown,
            summarize_audio_samples,
        )

        samples = analyze_audio_samples(args.path)
        if args.json:
            payload = {
                "summary": summarize_audio_samples(samples),
                "samples": [sample.to_dict() for sample in samples],
            }
            text = json.dumps(payload, ensure_ascii=False, indent=2)
        else:
            text = format_audio_samples_markdown(samples)

        if args.output:
            with open(args.output, "w", encoding="utf-8", newline="\n") as file:
                file.write(text)
        else:
            print(text)
        return
    if args.debug_command == "afe-ab":
        from .internal.ops.voice_ab import (
            VoiceAbTrial,
            collect_trial,
            format_voice_ab_markdown,
            reset_afe_counters,
            set_afe_bridge,
            stop_afe_shadow,
            wait_for_new_turn,
        )

        firmware_url = args.firmware_url or os.environ.get("NOISEBOT_ROBOT_HTTP_URL", "")
        if not firmware_url:
            host = args.host or os.environ.get("NOISEBOT_HOST", "")
            if host:
                firmware_url = f"http://{host}"
        if not firmware_url:
            raise SystemExit("--firmware-url ou --host/NOISEBOT_HOST e obrigatorio")

        server_url = args.server_url.rstrip("/")
        firmware_url = firmware_url.rstrip("/")
        modes = ["raw", "afe"] * max(1, args.repeat)
        trials: list[VoiceAbTrial] = []
        print(f"Frase alvo: {args.phrase}")
        print("Use a mesma distancia e volume em todos os turnos.")
        try:
            for index, mode in enumerate(modes, start=1):
                reset_afe_counters(firmware_url)
                before = collect_trial(
                    mode=mode,
                    phrase=args.phrase,
                    server_url=server_url,
                    firmware_url=firmware_url,
                )
                if mode == "afe":
                    set_afe_bridge(firmware_url, True)
                else:
                    set_afe_bridge(firmware_url, False)
                    try:
                        stop_afe_shadow(firmware_url)
                    except Exception:
                        pass
                print(f"\n[{index}/{len(modes)}] Modo {mode.upper()}")
                input("Fale a frase agora e pressione Enter quando o robô terminar: ")
                metrics = wait_for_new_turn(
                    server_url=server_url,
                    previous_turn_id=before.turn_id,
                    timeout_s=args.timeout_s,
                )
                processor = collect_trial(
                    mode=mode,
                    phrase=args.phrase,
                    server_url=server_url,
                    firmware_url=firmware_url,
                )
                trial = VoiceAbTrial.from_payload(
                    mode=mode,
                    phrase=args.phrase,
                    metrics=metrics,
                    processor=processor.to_dict(),
                )
                trials.append(trial)
                print(
                    f"turn={trial.turn_id} quality={trial.transcript_quality} "
                    f"no_speech={trial.no_speech_prob} transcript={trial.transcript!r}"
                )
        finally:
            try:
                set_afe_bridge(firmware_url, False)
                stop_afe_shadow(firmware_url)
            except Exception:
                pass

        if args.json:
            text = json.dumps([trial.to_dict() for trial in trials], ensure_ascii=False, indent=2)
        else:
            text = format_voice_ab_markdown(trials)
        if args.output:
            with open(args.output, "w", encoding="utf-8", newline="\n") as file:
                file.write(text)
        else:
            print(text)
        return
    if args.debug_command == "opus-selftest":
        import math

        import numpy as np

        from .internal.transport.opus_codec import (
            OPUS_FRAME_MS,
            OPUS_FRAME_SAMPLES,
            OPUS_SAMPLE_RATE_HZ,
            roundtrip_pcm,
        )

        n_samples = max(OPUS_FRAME_SAMPLES, int(args.seconds * OPUS_SAMPLE_RATE_HZ))
        t = np.arange(n_samples, dtype=np.float32) / float(OPUS_SAMPLE_RATE_HZ)
        pcm = (np.sin(2.0 * math.pi * 440.0 * t) * 8000.0).astype(np.int16)
        decoded, stats = roundtrip_pcm(pcm, bitrate=args.bitrate)
        payload = {
            "ok": True,
            "sample_rate_hz": OPUS_SAMPLE_RATE_HZ,
            "frame_ms": OPUS_FRAME_MS,
            "frame_samples": OPUS_FRAME_SAMPLES,
            "bitrate": args.bitrate,
            "input_bytes": stats.input_bytes,
            "packet_count": stats.packet_count,
            "opus_bytes": stats.opus_bytes,
            "decoded_bytes": len(decoded),
            "compression_ratio": round(stats.compression_ratio, 4),
        }
        if args.json:
            print(json.dumps(payload, ensure_ascii=False, indent=2))
        else:
            print(
                "Opus OK: "
                f"{payload['packet_count']} packets, "
                f"{payload['opus_bytes']} bytes de {payload['input_bytes']} "
                f"({payload['compression_ratio']:.2%}), "
                f"decoded={payload['decoded_bytes']} bytes"
            )
        return
    if args.debug_command == "opus-live":
        from .internal.ops.opus_live import (
            format_opus_live_json,
            format_opus_live_markdown,
            run_opus_live_trial,
        )

        firmware_url = args.firmware_url or os.environ.get("NOISEBOT_ROBOT_HTTP_URL", "")
        if not firmware_url:
            host = args.host or os.environ.get("NOISEBOT_HOST", "")
            if host:
                firmware_url = f"http://{host}"
        if not firmware_url:
            raise SystemExit("--firmware-url ou --host/NOISEBOT_HOST e obrigatorio")

        trial = run_opus_live_trial(
            phrase=args.phrase,
            server_url=args.server_url,
            firmware_url=firmware_url,
            timeout_s=args.timeout_s,
        )
        text = format_opus_live_json(trial) if args.json else format_opus_live_markdown(trial)
        if args.output:
            with open(args.output, "w", encoding="utf-8", newline="\n") as file:
                file.write(text)
        else:
            print(text)
        if not trial.ok:
            raise SystemExit(1)
        return
    if args.debug_command == "barge-live":
        from .internal.ops.barge_live import (
            format_barge_live_json,
            format_barge_live_markdown,
            run_barge_live_trial,
        )

        trial = run_barge_live_trial(
            phrase=args.phrase,
            server_url=args.server_url,
            timeout_s=args.timeout_s,
        )
        text = format_barge_live_json(trial) if args.json else format_barge_live_markdown(trial)
        if args.output:
            with open(args.output, "w", encoding="utf-8", newline="\n") as file:
                file.write(text)
        else:
            print(text)
        if not trial.ok:
            raise SystemExit(1)
        return
    raise SystemExit(2)


def run_service_command(args: argparse.Namespace) -> None:
    """Run server-owned service commands."""
    from .internal.service.manager import get_manager

    manager = get_manager()
    try:
        if args.service_command == "install":
            manager.install()
        elif args.service_command == "uninstall":
            manager.uninstall()
        elif args.service_command == "status":
            print(manager.status())
        elif args.service_command == "start":
            manager.start()
            print("Servico iniciado.")
        elif args.service_command == "stop":
            manager.stop()
            print("Servico parado.")
        else:
            raise SystemExit(2)
    except Exception as exc:
        print(f"Erro: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc


def main(argv: Sequence[str] | None = None) -> None:
    args = parse_args(argv)

    if args.command == "service":
        run_service_command(args)
        return
    if args.command == "debug":
        run_debug_command(args)
        return

    apply_env_overrides(args)
    env_file = find_env_file(args.env)
    config = load_config(env_file)
    run_server(config, log_file=args.log_file)
