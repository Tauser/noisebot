from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
COMPONENTS = ROOT / "components"


def test_wake_threshold_matches_validated_product_gate():
    wake_c = (COMPONENTS / "services" / "wake_service" / "wake_service.c").read_text(
        encoding="utf-8"
    )

    assert "#define WAKE_WAKENET_THRESHOLD 0.55f" in wake_c


def test_wake_diagnostic_endpoint_exposes_gate_state_and_last_stats():
    wake_h = (COMPONENTS / "services" / "wake_service" / "wake_service.h").read_text(
        encoding="utf-8"
    )
    wake_c = (COMPONENTS / "services" / "wake_service" / "wake_service.c").read_text(
        encoding="utf-8"
    )
    web = (ROOT / "components" / "infra" / "web_service.c").read_text(
        encoding="utf-8"
    )

    assert "bool wake_service_is_armed(void);" in wake_h
    assert "bool wake_service_is_suspended(void);" in wake_h
    assert "bool wake_service_is_armed(void)" in wake_c
    assert "bool wake_service_is_suspended(void)" in wake_c
    assert '{ .uri = "/api/diag/test/wake"' in web
    assert '\\"armed\\":%s' in web
    assert '\\"suspended\\":%s' in web
    assert '\\"last_raw_rms\\":%lu' in web
    assert '\\"last_raw_peak\\":%u' in web
    assert '\\"last_post_peak\\":%u' in web
    assert "wake_service_get_last_detection_stats(&stats)" in web
