from __future__ import annotations

import argparse
import json
import logging
import time

from .config import BridgeConfig
from .intent_router import LocalIntentRouter
from .llm import FallbackLlmProvider, create_llm_provider
from .replay import run_replay
from .runtime import BridgeRuntime
from .stt import WhisperStt
from .transport import TcpTransport, UartTransport, discover_mdns, do_handshake
from .tts import PiperTts


def configure_logging():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(name)s: %(message)s")


def parse_args() -> BridgeConfig:
    parser = argparse.ArgumentParser(description="NoiseBot LLM Bridge")
    parser.add_argument("--host", default=None, help="IP ou hostname do ESP32")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--uart", default=None, metavar="PORT", help="Porta serial USB CDC")
    parser.add_argument("--dry-run", action="store_true", help="Transcreve com Whisper e não chama LLM/Piper")
    parser.add_argument("--replay", default=None, help="Arquivo WAV/PCM int16 16kHz para testar sem hardware")
    parser.add_argument("--replay-json", action="store_true", help="Imprime resultado estruturado do replay em JSON")
    parser.add_argument("--local-intents", choices=("on", "off"), default="on")
    parser.add_argument("--llm", choices=("gemini", "openai", "mock", "none"), default="gemini")
    parser.add_argument("--fallback-llm", choices=("gemini", "openai", "mock", "none"), default="none")
    parser.add_argument("--whisper-model", default=BridgeConfig.whisper_model, help="Modelo Whisper local")
    parser.add_argument(
        "--whisper-backend",
        choices=("openai", "faster"),
        default=BridgeConfig.whisper_backend,
        help="Backend STT: openai-whisper ou faster-whisper",
    )
    args = parser.parse_args()
    return BridgeConfig(
        host=args.host,
        port=args.port,
        uart=args.uart,
        dry_run=args.dry_run,
        replay=args.replay,
        replay_json=args.replay_json,
        local_intents=args.local_intents == "on",
        llm=args.llm,
        fallback_llm=args.fallback_llm,
        whisper_model=args.whisper_model,
        whisper_backend=args.whisper_backend,
    )


def main():
    configure_logging()
    log = logging.getLogger("noisebot_bridge")
    cfg = parse_args()

    stt = WhisperStt(cfg.whisper_model, cfg.whisper_backend, cfg.whisper_device, cfg.whisper_compute_type)
    stt.init()

    if cfg.dry_run:
        log.info("DRY-RUN ativo — LLM/Piper desabilitados")
        llm = create_llm_provider("none")
    else:
        llm = create_llm_provider(cfg.llm)
        llm.init()
        if cfg.fallback_llm != "none":
            fallback_llm = create_llm_provider(cfg.fallback_llm)
            fallback_llm.init()
            llm = FallbackLlmProvider(llm, fallback_llm)
        if not llm.ready and cfg.llm != "none":
            log.warning("LLM %s indisponível — bridge seguirá em modo degradado/local-only", cfg.llm)

    tts = PiperTts(cfg.piper_model)
    intent_router = LocalIntentRouter() if cfg.local_intents else None

    if cfg.replay:
        result = run_replay(cfg.replay, stt, llm, tts, dry_run=cfg.dry_run, intent_router=intent_router)
        if cfg.replay_json:
            print(json.dumps(result.to_dict(), ensure_ascii=False, sort_keys=True))
        return

    while True:
        transport = None
        try:
            if cfg.uart:
                log.info("Conectando via UART %s", cfg.uart)
                transport = UartTransport(cfg.uart)
            else:
                host = cfg.host or discover_mdns() or "noisebot.local"
                log.info("Conectando TCP %s:%d", host, cfg.port)
                transport = TcpTransport(host, cfg.port)
                if not transport.connect():
                    log.info("Retentando em %.0fs...", cfg.reconnect_delay_s)
                    time.sleep(cfg.reconnect_delay_s)
                    continue

            if not do_handshake(transport):
                log.warning("Handshake falhou — retentando")
                transport.close()
                time.sleep(2)
                continue

            log.info("Handshake OK")
            runtime = BridgeRuntime(transport, stt, llm, tts, dry_run=cfg.dry_run, intent_router=intent_router)
            runtime.run()
        except KeyboardInterrupt:
            log.info("Encerrando bridge")
            if transport is not None:
                transport.close()
            break
        finally:
            if transport is not None:
                transport.close()
        log.info("Reconectando em %.0fs...", cfg.reconnect_delay_s)
        time.sleep(cfg.reconnect_delay_s)
