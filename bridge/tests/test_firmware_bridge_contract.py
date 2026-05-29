import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BRIDGE_SERVICE = ROOT / "components" / "infra" / "bridge_service.c"


def _firmware_hello() -> dict:
    src = BRIDGE_SERVICE.read_text(encoding="utf-8")
    match = re.search(r"BRIDGE_HELLO_V2\[\]\s*=\s*(.*?);", src, re.DOTALL)
    assert match is not None
    payload = "".join(re.findall(r'"((?:\\.|[^"\\])*)"', match.group(1)))
    payload = bytes(payload, "utf-8").decode("unicode_escape")
    return json.loads(payload)


def test_firmware_hello_keeps_pcm16_default_and_opus_disabled():
    hello = _firmware_hello()

    assert hello["protocol"] == "noisebot-bridge"
    assert hello["version"] == 2
    assert hello["role"] == "firmware"
    assert hello["audio"] == {
        "format": "pcm16",
        "sample_rate": 16000,
        "channels": 1,
        "chunk_samples": 256,
    }
    assert hello["codecs"] == {"pcm16": True, "opus": False}
