from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BEHAVIOR_ENGINE = ROOT / "components" / "behavior" / "behavior_engine" / "behavior_engine.c"


def _source() -> str:
    return BEHAVIOR_ENGINE.read_text(encoding="utf-8")


def test_speech_cancel_clears_stale_text_overlay_before_listening():
    src = _source()

    start = src.index("case NB_EVT_BRIDGE_SPEECH_CANCEL:")
    end = src.index("NB_LOGI(TAG, \"SPEECH_CANCEL aplicado", start)
    block = src[start:end]

    assert "audio_play_stop();" in block
    assert "ui_overlay_clear_text();" in block
    assert block.index("ui_overlay_clear_text();") < block.index("ui_overlay_listening_set(true);")


def test_listen_start_clears_stale_text_overlay():
    src = _source()

    start = src.index('if (strstr(payload, "\\"event\\":\\"LISTEN_START\\""))')
    end = src.index('} else if (strstr(payload, "\\"event\\":\\"LISTEN_STOP\\"")', start)
    block = src[start:end]

    assert "ui_overlay_clear_text();" in block
    assert block.index("ui_overlay_clear_text();") < block.index("ui_overlay_listening_set(true);")


def test_bridge_led_command_controls_led_service():
    src = _source()

    assert "static bool session_json_string_equals(" in src

    start = src.index('} else if (session_json_string_equals(payload, "event", "LED_COMMAND"))')
    end = src.index("}\n        }\n        break;", start)
    block = src[start:end]

    assert 'session_json_string_equals(payload, "action", "reset")' in block
    assert "led_base_set(NB_LED_BASE_IDLE, true);" in block
    assert 'session_json_u8(payload, "r", &red)' in block
    assert 'session_json_u8(payload, "g", &green)' in block
    assert 'session_json_u8(payload, "b", &blue)' in block
    assert "led_set_all(color);" in block


def test_bridge_settings_command_uses_json_field_parser():
    src = _source()

    start = src.index('} else if (session_json_string_equals(payload, "event", "SETTINGS_COMMAND"))')
    end = src.index('} else if (session_json_string_equals(payload, "event", "LED_COMMAND"))', start)
    block = src[start:end]

    assert 'session_json_u8(payload, "display_brightness", &brightness)' in block
    assert 'session_json_u8(payload, "led_brightness", &brightness)' in block


def test_config_brightness_endpoint_applies_runtime_services():
    src = (ROOT / "components" / "infra" / "web_service.c").read_text(encoding="utf-8")

    start = src.index('else if (strcmp(key, "brightness")')
    end = src.index('else if (strcmp(key, "touch_sens")', start)
    block = src[start:end]

    assert "config_set_brightness(brightness)" in block
    assert "render_service_set_brightness(brightness);" in block
    assert "led_set_brightness(brightness);" in block
