"""bridgev2.debug.manual -- comandos manuais para validar o bridge v2.

Estes comandos existem para tornar o fake_firmware e a injeção de transcript
usáveis fora da suíte automatizada.
"""
from __future__ import annotations

import asyncio
import json
import logging
from typing import Any

from .fake_firmware import FakeFirmware
from ..runtime.bus import EventBus
from ..runtime.events import FinalTranscript, IntentResolved, RobotCommand, SpeechDone
from ..runtime.orchestrator import Orchestrator
from ..protocol.messages import (
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

log = logging.getLogger(__name__)


_MSG_NAMES: dict[int, str] = {
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


def _msg_name(msg_type: int) -> str:
    return _MSG_NAMES.get(msg_type, f"0x{msg_type:02X}")


async def run_transcript_debug(text: str, turn_id: int = 1) -> int:
    """Injeta um FinalTranscript sintético e imprime intent/comandos gerados."""
    bus = EventBus(default_maxsize=128)
    orchestrator = Orchestrator(bus, get_adapter=lambda: None, stt_provider=None)
    q = bus.subscribe(IntentResolved, RobotCommand, SpeechDone)
    task = asyncio.create_task(orchestrator.run(), name="debug_transcript_orch")

    try:
        await bus.publish(FinalTranscript(turn_id=turn_id, text=text))
        intent: IntentResolved | None = None
        commands: list[RobotCommand] = []

        while True:
            event = await asyncio.wait_for(q.get(), timeout=3.0)
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
                {"kind": c.kind, "payload": c.payload, "turn_id": c.turn_id}
                for c in commands
            ],
        }
        print(json.dumps(payload, ensure_ascii=False, indent=2))
        return 0
    finally:
        await orchestrator.shutdown()
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)


async def run_fake_firmware_debug(
    host: str,
    port: int,
    features: list[str],
    auto_silence_chunks: int,
) -> int:
    """Sobe o fake firmware e opcionalmente injeta uma sessão de silêncio."""
    fw = FakeFirmware(host=host, port=port, firmware_features=features)
    await fw.start()
    print(f"fake_firmware ouvindo em {host}:{port}")
    print("rode o bridge em outro terminal, por exemplo:")
    print(f"  python -m bridgev2 --host {host} --port {port} --log-level DEBUG")

    try:
        if not await fw.wait_connected(timeout=120.0):
            print("timeout aguardando bridge conectar")
            return 1

        print("bridge conectado")
        if auto_silence_chunks > 0:
            print(f"injetando sessão: VOICE_START + {auto_silence_chunks} chunks silenciosos + VOICE_END")
            await fw.send_voice_start()
            await fw.send_audio_chunks(auto_silence_chunks)
            await fw.send_voice_end()

        seen = 0
        while True:
            await asyncio.sleep(0.25)
            frames = fw.received_types()
            if len(frames) == seen:
                continue
            for msg_type in frames[seen:]:
                print(f"bridge -> fake_fw: {_msg_name(msg_type)}")
            seen = len(frames)
    except KeyboardInterrupt:
        return 0
    finally:
        await fw.stop()
