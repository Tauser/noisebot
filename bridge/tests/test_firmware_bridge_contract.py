import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BRIDGE_SERVICE = ROOT / "components" / "infra" / "bridge_service.c"
BRIDGE_SERVICE_H = ROOT / "components" / "infra" / "bridge_service.h"


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


def test_firmware_opus_contract_is_explicit_but_disabled():
    header = BRIDGE_SERVICE_H.read_text(encoding="utf-8")
    src = BRIDGE_SERVICE.read_text(encoding="utf-8")

    assert "#define NB_BRIDGE_OPUS_FRAME_MS       60" in header
    assert "#define NB_BRIDGE_OPUS_FRAME_SAMPLES  960" in header
    assert "#define NB_BRIDGE_OPUS_MAX_PACKET_BYTES 1024" in header
    assert "bool bridge_service_opus_is_enabled(void);" in header
    assert "esp_err_t bridge_service_send_opus_packet(" in header
    assert "bool bridge_service_opus_is_enabled(void)\n{\n    return false;\n}" in src
    assert "if (!bridge_service_opus_is_enabled()) return ESP_ERR_NOT_SUPPORTED;" in src
    assert "enqueue_frame(NB_BRIDGE_MSG_AUDIO_CHUNK, packet, len)" in src
