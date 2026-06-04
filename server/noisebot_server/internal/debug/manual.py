"""Manual debug commands owned by ``noisebot_server``."""

from __future__ import annotations

import asyncio
import json
import os
from pathlib import Path
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

from ..agent.orchestrator import Orchestrator
from ..agent.runtime import (
    EventBus,
    FinalTranscript,
    IntentResolved,
    RobotCommand,
    SpeechDone,
)
from ..transport.protocol import (
    MSG_ACTION,
    MSG_EMOT_EVENT,
    MSG_EXPR,
    MSG_GAZE,
    MSG_HELLO,
    MSG_SAY,
    MSG_SAY_BEGIN,
    MSG_SAY_END,
    MSG_SESSION,
    MSG_SPEECH_CANCEL,
    MSG_STATUS,
    MSG_TEXT_SCROLL,
    MSG_VOLUME,
)
from .fake_firmware import FakeFirmware

MSG_NAMES: dict[int, str] = {
    MSG_HELLO: "HELLO",
    MSG_SAY: "SAY",
    MSG_EXPR: "EXPR",
    MSG_ACTION: "ACTION",
    MSG_EMOT_EVENT: "EMOT_EVENT",
    MSG_GAZE: "GAZE",
    MSG_TEXT_SCROLL: "TEXT_SCROLL",
    MSG_VOLUME: "VOLUME",
    MSG_STATUS: "STATUS",
    MSG_SESSION: "SESSION",
    MSG_SPEECH_CANCEL: "SPEECH_CANCEL",
    MSG_SAY_BEGIN: "SAY_BEGIN",
    MSG_SAY_END: "SAY_END",
}


def msg_name(msg_type: int) -> str:
    return MSG_NAMES.get(msg_type, f"0x{msg_type:02X}")


async def run_transcript_debug(text: str, turn_id: int = 1) -> int:
    """Inject a synthetic transcript and print generated intent/commands."""
    bus = EventBus(default_maxsize=128)
    orchestrator = Orchestrator(bus, get_adapter=lambda: None, stt_provider=None)
    queue = bus.subscribe(IntentResolved, RobotCommand, SpeechDone)
    task = asyncio.create_task(orchestrator.run(), name="debug_transcript_orch")

    try:
        await bus.publish(FinalTranscript(turn_id=turn_id, text=text))
        intent: IntentResolved | None = None
        commands: list[RobotCommand] = []

        while True:
            event = await asyncio.wait_for(queue.get(), timeout=3.0)
            if isinstance(event, IntentResolved):
                intent = event
            elif isinstance(event, RobotCommand):
                commands.append(event)
            elif isinstance(event, SpeechDone):
                break

        payload: dict[str, Any] = {
            "turn_id": turn_id,
            "text": text,
            "intent": intent.intent_name if intent else None,
            "reply": intent.reply_text if intent else None,
            "commands": [
                {
                    "kind": command.kind,
                    "payload": command.payload,
                    "turn_id": command.turn_id,
                }
                for command in commands
            ],
        }
        print(json.dumps(payload, ensure_ascii=False, indent=2))
        return 0
    finally:
        await orchestrator.shutdown()
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)


def run_live_transcript_debug(
    text: str,
    server_url: str = "http://127.0.0.1:8765",
    turn_id: int = 0,
    token: str = "",
    emit_json: bool = False,
) -> int:
    """Inject a transcript through the running Ops HTTP server."""
    resolved_token = _resolve_ops_token(token)
    if not resolved_token:
        print(
            "token ausente: informe --token, configure NOISEBOT_OPS_TOKEN "
            "ou verifique ~/.noisebot-server/ops_token"
        )
        return 1

    payload: dict[str, Any] = {"text": text}
    if turn_id > 0:
        payload["turn_id"] = turn_id

    url = server_url.rstrip("/") + "/debug/transcript"
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    request = Request(
        url,
        data=body,
        method="POST",
        headers={
            "Authorization": f"Bearer {resolved_token}",
            "Content-Type": "application/json",
            "User-Agent": "NoiseBot-Server/0.1",
        },
    )

    try:
        with urlopen(request, timeout=5.0) as response:
            response_body = response.read().decode("utf-8")
            status = getattr(response, "status", 200)
    except HTTPError as exc:
        response_body = exc.read().decode("utf-8", errors="replace")
        status = exc.code
    except (URLError, TimeoutError, OSError) as exc:
        print(f"falha ao chamar {url}: {exc}")
        return 1

    try:
        decoded = json.loads(response_body)
    except json.JSONDecodeError:
        decoded = {"ok": False, "status": status, "body": response_body}

    if emit_json:
        print(json.dumps(decoded, ensure_ascii=False, indent=2))
    else:
        ok = bool(decoded.get("status") == "ok" or decoded.get("ok"))
        turn = decoded.get("turn_id", payload.get("turn_id", "auto"))
        if ok and status < 400:
            print(f"transcript injetado: turn_id={turn} text={text!r}")
        else:
            print(json.dumps(decoded, ensure_ascii=False, indent=2))

    return 0 if status < 400 else 1


def _resolve_ops_token(token: str) -> str:
    if token.strip():
        return token.strip()
    env_token = os.environ.get("NOISEBOT_OPS_TOKEN", "").strip()
    if env_token:
        return env_token
    token_file = Path.home() / ".noisebot-server" / "ops_token"
    try:
        return token_file.read_text(encoding="utf-8").strip()
    except OSError:
        return ""


async def run_fake_firmware_debug(
    host: str,
    port: int,
    features: list[str],
    audio_format: str,
    auto_silence_chunks: int,
) -> int:
    """Run the fake firmware and optionally inject a silent voice session."""
    firmware = FakeFirmware(
        host=host,
        port=port,
        firmware_features=features,
        audio_format=audio_format,
    )
    await firmware.start()
    print(f"fake_firmware ouvindo em {host}:{port}")
    print(f"audio_format={audio_format}")
    print("rode o server em outro terminal, por exemplo:")
    print(f"  python -m noisebot_server --host {host} --port {port} --log-level DEBUG")

    try:
        if not await firmware.wait_connected(timeout=120.0):
            print("timeout aguardando server conectar")
            return 1

        print("server conectado")
        if auto_silence_chunks > 0:
            print(
                "injetando sessão: VOICE_START + "
                f"{auto_silence_chunks} chunks silenciosos + VOICE_END"
            )
            await firmware.send_voice_start()
            await firmware.send_audio_chunks(auto_silence_chunks)
            await firmware.send_voice_end()

        seen = 0
        while True:
            await asyncio.sleep(0.25)
            frames = firmware.received_types()
            if len(frames) == seen:
                continue
            for msg_type in frames[seen:]:
                print(f"server -> fake_fw: {msg_name(msg_type)}")
            seen = len(frames)
    except KeyboardInterrupt:
        return 0
    finally:
        await firmware.stop()
