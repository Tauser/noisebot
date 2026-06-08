import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BRIDGE_SERVICE = ROOT / "components" / "infra" / "bridge_service.c"
BRIDGE_SERVICE_H = ROOT / "components" / "infra" / "bridge_service.h"
AUDIO_PROCESSOR = ROOT / "components" / "services" / "audio_processor_service" / "audio_processor_service.c"
WEB_SERVICE = ROOT / "components" / "infra" / "web_service.c"


def _firmware_hello() -> dict:
    src = BRIDGE_SERVICE.read_text(encoding="utf-8")
    match = re.search(r"BRIDGE_HELLO_V2\[\]\s*=\s*(.*?);", src, re.DOTALL)
    assert match is not None
    payload = "".join(re.findall(r'"((?:\\.|[^"\\])*)"', match.group(1)))
    payload = bytes(payload, "utf-8").decode("unicode_escape")
    return json.loads(payload)


def _firmware_opus_hello() -> dict:
    src = BRIDGE_SERVICE.read_text(encoding="utf-8")
    match = re.search(r"BRIDGE_HELLO_V2_OPUS\[\]\s*=\s*(.*?);", src, re.DOTALL)
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
    assert hello["codec_options"] == {
        "opus_tx": True,
        "opus_default": False,
        "opus_sample_rate": 16000,
        "opus_channels": 1,
        "opus_frame_duration": 60,
        "opus_frame_samples": 960,
        "opus_bitrate": 32000,
    }


def test_firmware_opus_contract_is_explicit_but_disabled():
    header = BRIDGE_SERVICE_H.read_text(encoding="utf-8")
    src = BRIDGE_SERVICE.read_text(encoding="utf-8")
    hello = _firmware_opus_hello()

    assert "#define NB_BRIDGE_OPUS_FRAME_MS       60" in header
    assert "#define NB_BRIDGE_OPUS_FRAME_SAMPLES  960" in header
    assert "#define NB_BRIDGE_OPUS_MAX_PACKET_BYTES 1024" in header
    assert "bool bridge_service_opus_is_enabled(void);" in header
    assert "esp_err_t bridge_service_send_opus_packet(" in header
    assert "static bool s_opus_enabled = false;" in src
    assert hello["audio"] == {
        "format": "opus",
        "sample_rate": 16000,
        "channels": 1,
        "chunk_samples": 960,
        "frame_duration": 60,
    }
    assert hello["codecs"] == {"pcm16": False, "opus": True}
    assert hello["codec_options"]["opus_tx"] is True
    assert hello["codec_options"]["opus_default"] is False
    assert hello["codec_options"]["opus_frame_duration"] == 60
    assert hello["codec_options"]["opus_frame_samples"] == 960
    assert hello["codec_options"]["opus_bitrate"] == 32000
    assert "opus_tx" in hello["features"]
    assert "void bridge_service_set_opus_enabled(bool enabled)" in src
    assert "bridge_service_opus_is_enabled() ? BRIDGE_HELLO_V2_OPUS : BRIDGE_HELLO_V2" in src
    assert "if (!bridge_service_opus_is_enabled()) return ESP_ERR_NOT_SUPPORTED;" in src
    assert "enqueue_frame(NB_BRIDGE_MSG_AUDIO_CHUNK, packet, len)" in src


def test_firmware_opus_encoder_uses_live_quality_profile():
    processor = AUDIO_PROCESSOR.read_text(encoding="utf-8")
    web = WEB_SERVICE.read_text(encoding="utf-8")

    assert "#define OPUS_FRAME_DURATION_MS   60" in processor
    assert "#define OPUS_TARGET_BITRATE      32000" in processor
    assert ".frame_duration     = ESP_OPUS_ENC_FRAME_DURATION_60_MS" in processor
    assert ".bitrate            = OPUS_TARGET_BITRATE" in processor
    assert '\\"bitrate\\":%d' in web
