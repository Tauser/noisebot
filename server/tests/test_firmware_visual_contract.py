from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BEHAVIOR_ENGINE = ROOT / "components" / "behavior" / "behavior_engine" / "behavior_engine.c"
UI_OVERLAY = ROOT / "components" / "services" / "ui_overlay_service" / "ui_overlay_service.cpp"
UI_OVERLAY_CMAKE = ROOT / "components" / "services" / "ui_overlay_service" / "CMakeLists.txt"
UI_OVERLAY_ASSETS = (
    ROOT
    / "components"
    / "services"
    / "ui_overlay_service"
    / "ui_overlay_assets.h"
)
BOOT_MANAGER = ROOT / "components" / "infra" / "boot_manager.c"
NB_CONFIG_KEYS = ROOT / "components" / "infra" / "nb_config_keys.h"
EXPRESSION_SERVICE = (
    ROOT
    / "components"
    / "services"
    / "expression_service"
    / "expression_service.cpp"
)
TITLE_FONT = (
    ROOT
    / "components"
    / "services"
    / "ui_overlay_service"
    / "fonts"
    / "MontserratSemiBold26.c"
)
MONTSERRAT_PTBR_FONT = (
    ROOT
    / "components"
    / "services"
    / "ui_overlay_service"
    / "fonts"
    / "MontserratPtBr16.c"
)


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

    assert 'session_json_u8(payload, "led_brightness", &brightness)' in block
    assert 'session_json_u8(payload, "display_brightness", &brightness)' not in block


def test_config_brightness_endpoint_applies_led_runtime_only():
    src = (ROOT / "components" / "infra" / "web_service.c").read_text(encoding="utf-8")

    start = src.index('else if (strcmp(key, "brightness")')
    end = src.index('else if (strcmp(key, "touch_sens")', start)
    block = src[start:end]

    assert "config_set_brightness(brightness)" in block
    assert "led_set_brightness(brightness);" in block
    assert "render_service_set_brightness(brightness);" not in block


def test_touch_boot_uses_persisted_sensitivity():
    src = BOOT_MANAGER.read_text(encoding="utf-8")

    assert "config_get_touch_sensitivity()" in src
    assert "touch_sens_pct * 0.2f" in src
    assert "touch_service_set_sensitivity(sens_factor);" in src
    assert "sens_factor = 0.5f" not in src


def test_touch_default_is_conservative_for_copper_pad():
    src = NB_CONFIG_KEYS.read_text(encoding="utf-8")

    assert "#define NB_CFG_DEFAULT_TOUCH_SENS        25" in src
    assert "passos de 0,2%" in src


def test_touch_legacy_default_migrates_to_conservative_threshold():
    src = (ROOT / "components" / "infra" / "config_manager.c").read_text(encoding="utf-8")

    assert "restore_touch_sensitivity_if_legacy()" in src
    assert "touch_sens != 10U" in src
    assert "NB_CFG_DEFAULT_TOUCH_SENS" in src


def test_touch_current_pad_uses_stable_press_timing():
    src = (ROOT / "components" / "services" / "touch_service" / "touch_service.c").read_text(
        encoding="utf-8"
    )

    assert "#define LONG_PRESS_MS        1500U" in src
    assert "#define DEBOUNCE_ON_COUNT      3U" in src
    assert "#define DEBOUNCE_OFF_COUNT     3U" in src
    assert "controladores capacitivos dedicados" in src


def test_touch_long_press_is_affection_not_startle_by_default():
    src = BEHAVIOR_ENGINE.read_text(encoding="utf-8")

    start = src.index("{ NB_EVT_TOUCH_LONG_PRESS")
    end = src.index("}},", start)
    block = src[start:end]

    assert "ACT_EMOT(TOUCH_LONG)" in block
    assert "ACT_PLAY(TOUCH_WARM)" in block
    assert "ACT_PLAY(TOUCH_STARTLE)" not in block


def test_speaking_mouth_uses_viseme_poses_from_closed_base():
    src = EXPRESSION_SERVICE.read_text(encoding="utf-8")

    assert "SPEAKING_EYE_Y_NORM  = -0.55f" in src
    assert "SPEAKING_EYE_OPEN    = 0.70f" in src
    assert "MOUTH_CENTER_X       = 160" in src
    assert "MOUTH_CENTER_Y       = 120" in src
    assert "MOUTH_REF_OFF_X      = 0" in src
    assert "MOUTH_REF_OFF_Y      = 44" in src
    assert "MOUTH_BASE_W         = 90" in src
    assert "MOUTH_BASE_H         = 6" in src
    assert "MOUTH_TOP_Y          = MOUTH_CENTER_Y + MOUTH_REF_OFF_Y - (MOUTH_BASE_H / 2)" in src
    assert "MOUTH_CORNER_RADIUS  = 3" in src
    assert "MOUTH_TALK_INTERVAL_US = 115000LL" in src
    assert "MOUTH_TALK_EASE_US     = 68000LL" in src
    assert "typedef struct {" in src
    assert "nb_mouth_pose_t" in src
    assert "static constexpr nb_mouth_pose_t MOUTH_POSES[] = {" in src
    assert "{90,  6, 1.0f}" in src   # closed — 3-field struct (w, h, hold)
    assert "{54, 24, 1.5f}" in src   # open (vogal tônica)
    assert "{49, 19, 1.1f}" in src   # arredond (O/U)
    assert "MOUTH_CLOSED_POSE = 0" in src
    assert "static constexpr nb_mouth_phrase_t MOUTH_PHRASES[] = {" in src
    assert "MOUTH_PHRASE_COUNT = 4" in src
    assert "float y_l = clamp_abs(face.y_l + gy + dy_l_ovl, GAZE_Y_MAX);" in src
    assert "float y_r = clamp_abs(face.y_r + gy + dy_r_ovl, GAZE_Y_MAX);" in src
    assert "s_speaking_mouth_from_pose = s_speaking_mouth_to_pose;" in src
    assert "s_speaking_mouth_to_pose = next;" in src
    assert "esp_random() % (MOUTH_POSE_COUNT - 1U)" not in src
    assert "nb_mouth_pose_t from = MOUTH_POSES[s_speaking_mouth_from_pose];" in src
    assert "nb_mouth_pose_t to   = MOUTH_POSES[s_speaking_mouth_to_pose];" in src
    assert "int16_t mouth_y = MOUTH_TOP_Y;" in src
    assert "mouth_offset_y" not in src
    assert "edge_color" not in src
    assert "spr->fillRoundRect(mouth_x - 1, mouth_y - 1, mouth_w + 2, mouth_h + 2" not in src
    assert "spr->fillRoundRect(mouth_x, mouth_y, mouth_w, mouth_h, MOUTH_CORNER_RADIUS, color);" in src
    assert "MOUTH_MAX_RADIUS" not in src
    assert "s_speaking_mouth_from_weight" not in src
    assert "visual_radius" not in src
    assert "inner_x" not in src


def test_touch_config_applies_runtime_sensitivity():
    src = (ROOT / "components" / "infra" / "web_service.c").read_text(encoding="utf-8")

    start = src.index('else if (strcmp(key, "touch_sens")')
    end = src.index('else if (strcmp(key, "idle_timeout")', start)
    block = src[start:end]

    assert "config_set_touch_sensitivity(touch_sens_pct)" in block
    assert "touch_service_set_sensitivity" in block
    assert "touch_sens_pct * 0.2f" in block


def test_touch_task_uses_internal_stack():
    src = BOOT_MANAGER.read_text(encoding="utf-8")

    start = src.index("touch_update_task")
    start = src.index("touch_service_set_event_cb(on_touch_event);", start)
    end = src.index('NB_ASSERT(rc == pdPASS, TAG, "xTaskCreate touch_task falhou");', start)
    block = src[start:end]

    assert "xTaskCreate(touch_update_task" in block
    assert "xTaskCreateWithCaps(touch_update_task" not in block
    assert "MALLOC_CAP_SPIRAM" not in block


def test_ui_overlay_uses_custom_montserrat_font_family():
    src = UI_OVERLAY.read_text(encoding="utf-8")
    assets = UI_OVERLAY_ASSETS.read_text(encoding="utf-8")
    cmake = UI_OVERLAY_CMAKE.read_text(encoding="utf-8")
    font_src = TITLE_FONT.read_text(encoding="utf-8")

    assert '#include "ui_overlay_assets.h"' in src
    assert "extern const lv_font_t MontserratSemiBold26;" in assets
    assert "extern const lv_font_t MontserratPtBr16;" in assets
    assert "MontserratSemiBold26" in src
    assert "MontserratPtBr16" in src
    assert "fonts/MontserratSemiBold26.c" in cmake
    assert "fonts/MontserratPtBr16.c" in cmake
    assert "Montserrat-SemiBold.ttf" in font_src
    assert "lv_font_montserrat_14" in src
    assert "lv_font_montserrat_16" in src
    assert "lv_font_montserrat_48" in src
    assert "UI_FONT_TEXT  = &UI_FONT_MONTSERRAT_PTBR" in src
    assert "UI_FONT_TITLE = &UI_FONT_BRAND_TITLE" in src
    assert "ui_set_font(spr, UI_FONT_BODY);" in src
    assert "ui_set_font(spr, UI_FONT_TEXT);" in src
    assert "ui_set_font(spr, UI_FONT_TITLE);" in src


def test_text_overlay_preserves_utf8_without_manual_accent_marks():
    src = UI_OVERLAY.read_text(encoding="utf-8")

    assert "build_pt_display_text" not in src
    assert "draw_pt_marks" not in src
    assert "pt_char_from_codepoint" not in src
    assert "setAttribute(lgfx::attribute_t::utf8_switch, 1)" in src
    assert "efontCN_" not in src


def test_montserrat_ptbr_font_covers_latin1():
    font_src = MONTSERRAT_PTBR_FONT.read_text(encoding="utf-8")

    assert "const lv_font_t MontserratPtBr16" in font_src
    assert ".range_start = 32, .range_length = 95" in font_src
    assert ".range_start = 160, .range_length = 96" in font_src
    assert "0x20-0x7F,0xA0-0xFF" in font_src
