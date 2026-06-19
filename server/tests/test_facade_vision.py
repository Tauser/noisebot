from __future__ import annotations
import asyncio
import importlib
import io
import json
import logging
import math
import struct
from pathlib import Path
from urllib.error import HTTPError
import pytest

from _facade_common import _drain_queue, _make_server_config, _server_loud_pcm, _simulate_server_voice_session, _wait_until


def test_server_presence_poll_uses_explicit_firmware_http_url(monkeypatch) -> None:
    app_module = importlib.import_module("noisebot_server.app")

    monkeypatch.setenv("NOISEBOT_ROBOT_HTTP_URL", "http://192.168.1.30/")
    config = _make_server_config(host=None)

    assert app_module._firmware_http_base_url(config) == "http://192.168.1.30"


def test_server_vision_observation_parses_firmware_payload() -> None:
    vision = importlib.import_module("noisebot_server.internal.vision")

    observation = vision.VisionObservation.from_payload({
        "ok": True,
        "observation": {
            "valid": True,
            "scene": "normal",
            "timestamp_ms": 1234,
            "width": 640,
            "height": 480,
            "jpeg_bytes": 54233,
            "capture_ms": 897,
            "luma_avg": 122,
            "luma_min": 0,
            "luma_max": 255,
            "contrast": 255,
            "motion_score": 5,
        },
    })

    assert observation.valid is True
    assert observation.width == 640
    assert observation.height == 480
    assert observation.scene == "normal"

def test_server_vision_face_center_normalization() -> None:
    vision = importlib.import_module("noisebot_server.internal.vision")

    observation = vision.VisionObservation.from_payload({
        "valid": True,
        "scene": "normal",
        "width": 640,
        "height": 480,
    })
    face = vision.FaceBox(x=240, y=120, width=160, height=120)
    analysis = vision.VisionAnalysis(
        observation=observation,
        detector="test",
        detector_available=True,
        face_detected=True,
        face_count=1,
        primary_face=face,
    )

    assert analysis.face_center_norm_x == 0.0
    assert analysis.face_center_norm_y == -0.25

def test_firmware_vision_presence_contract_is_exposed() -> None:
    root = Path(__file__).resolve().parents[2] / "firmware" / "main-controller"
    events_h = (root / "components" / "infra" / "nb_events.h").read_text(encoding="utf-8")
    vision_h = (
        root
        / "components"
        / "services"
        / "vision_service"
        / "vision_service.h"
    ).read_text(encoding="utf-8")
    vision_c = (
        root
        / "components"
        / "services"
        / "vision_service"
        / "vision_service.c"
    ).read_text(encoding="utf-8")
    web_c = (root / "components" / "infra" / "web_service.c").read_text(encoding="utf-8")

    assert "NB_EVT_PRESENCE_DETECTED" in events_h
    assert "NB_EVT_PRESENCE_LOST" in events_h
    assert "nb_vision_presence_status_t" in vision_h
    assert "vision_service_evaluate_presence" in vision_h
    assert "vision_service_reset_presence" in vision_h
    assert "nb_vision_poll_status_t" in vision_h
    assert "vision_service_poll_start" in vision_h
    assert "vision_service_poll_stop" in vision_h
    assert "vision_service_get_poll_status" in vision_h
    assert "detected_event_count" in vision_h
    assert "lost_event_count" in vision_h
    assert "spatial_score" in vision_h
    assert "spatial_score" in (root / "components" / "services" / "camera_service" / "camera_service.h").read_text(encoding="utf-8")
    assert "NB_VISION_PRESENCE_LOST_MS 120000U" in vision_c
    assert "obs->spatial_score >=" not in vision_c
    assert "spatial_bonus" not in vision_c
    assert "nb_event_publish_async(&evt)" in vision_c
    assert "record_presence_event_publish" in vision_c
    assert "NB_VISION_PRESENCE_SCORE_STRONG 60U" in vision_c
    assert "NB_VISION_PRESENCE_RETAIN_MS 2500U" in vision_c
    assert "NB_VISION_PRESENCE_MIN_SAMPLES 2U" in vision_c
    assert "if (raw_candidate) {\n                status.stable_samples++;" in vision_c
    assert "raw_candidate &&\n                status.stable_samples" in vision_c
    assert "vision_poll_task" in vision_c
    assert "NB_VISION_POLL_MIN_INTERVAL_MS 250U" in vision_c
    assert "xTaskCreate(vision_poll_task" in vision_c
    assert "vision_service_reset_presence" in vision_c
    assert "camera_service_get_mode() != NB_CAMERA_MODE_SAFE_QQVGA" in vision_c
    assert "camera_service_set_mode(NB_CAMERA_MODE_SAFE_QQVGA)" in vision_c
    assert "camera_service_observe_scene(&cam_obs)" in vision_c
    assert "camera_service_capture_snapshot" not in vision_c
    assert ".jpeg_bytes = 0U" in vision_c
    assert "vision_presence_json" in web_c
    assert "vision_service_get_presence(&presence)" in web_c
    assert '"/api/vision/presence/reset"' in web_c
    assert "detected_event_count" in web_c
    assert "lost_event_count" in web_c
    assert '\\"spatial_score\\":%u' in web_c
    assert "vision_poll_json" in web_c
    assert '"/api/vision/poll/status"' in web_c
    assert '"/api/vision/poll/start"' in web_c
    assert '"/api/vision/poll/stop"' in web_c

def test_firmware_camera_hal_reports_honest_native_resolution() -> None:
    root = Path(__file__).resolve().parents[2] / "firmware" / "main-controller"
    camera_h = (
        root
        / "components"
        / "nb_hal"
        / "camera_hal.h"
    ).read_text(encoding="utf-8")
    camera_c = (
        root
        / "components"
        / "nb_hal"
        / "camera_hal.c"
    ).read_text(encoding="utf-8")

    assert "NB_CAMERA_MODE_SAFE_QQVGA" in camera_h
    assert "NB_CAMERA_MODE_BETTER_QVGA" in camera_h
    # Resolution is native/effective and fixed by whichever
    # CONFIG_CAMERA_OV2640_DVP_* Kconfig choice is compiled in — mode never
    # fakes a higher resolution, it only selects JPEG quality elsewhere. The
    # mode_width/height fallback constants must track the active Kconfig
    # choice (currently the final 240x240 YUV422 baseline after the failed
    # native-JPEG experiments) so /api/camera/status never reports a stale
    # native size before the first frame lands.
    assert "size_t camera_hal_mode_width(nb_camera_mode_t mode)" in camera_c
    assert "size_t camera_hal_mode_height(nb_camera_mode_t mode)" in camera_c
    assert "#define NB_CAMERA_NATIVE_FALLBACK_WIDTH  240U" in camera_c
    assert "#define NB_CAMERA_NATIVE_FALLBACK_HEIGHT 240U" in camera_c
    assert "return NB_CAMERA_NATIVE_FALLBACK_WIDTH;" in camera_c
    assert "return NB_CAMERA_NATIVE_FALLBACK_HEIGHT;" in camera_c
    assert "size_t camera_hal_effective_width(void)" in camera_h
    assert "size_t camera_hal_effective_height(void)" in camera_h
    assert "return s_frame_width;" in camera_c
    assert "return s_frame_height;" in camera_c
    assert "if (g_fmt_ok) {\n        fmt = current_fmt;" in camera_c
    assert "fmt.fmt.pix.width = 240U;" in camera_c
    assert "fmt.fmt.pix.height = 240U;" in camera_c
    assert "Build the VIDIOC_S_FMT request from VIDIOC_G_FMT's own struct" in camera_c
    assert "int camera_hal_last_sfmt_errno(void)" in camera_h
    assert "s_last_sfmt_errno = errno;" in camera_c
    # Real VIDIOC_TRY_FMT negotiation proof — logged at every boot, not assumed.
    assert "static void camera_hal_probe_try_fmt(int fd, uint32_t pixfmt" in camera_c
    assert "ioctl(fd, VIDIOC_TRY_FMT, &probe)" in camera_c
    assert "static void camera_hal_log_sensor_format(int fd)" in camera_c
    assert "ioctl(fd, VIDIOC_G_SENSOR_FMT, &sensor_fmt)" in camera_c

def test_firmware_camera_sensor_returns_to_validated_yuv422_baseline() -> None:
    root = Path(__file__).resolve().parents[2] / "firmware" / "main-controller"
    defaults = (root / "sdkconfig.defaults").read_text(encoding="utf-8")

    # 2026-06-07 high-resolution experiment: native JPEG modes
    # JPEG_640X480_25FPS and JPEG_320X240_50FPS were measured on hardware and
    # marked non-functional for this OV2640 DVP board. The validated baseline
    # is therefore back to 240x240 YUV422. Exactly one OV2640 DVP resolution
    # choice must be active — selecting more than one is a Kconfig
    # misconfiguration.
    assert "CONFIG_CAMERA_OV2640_DVP_YUV422_240X240_25FPS=y" in defaults
    assert "CONFIG_CAMERA_OV2640_DVP_JPEG_640X480_25FPS=y" not in defaults
    assert "CONFIG_CAMERA_OV2640_DVP_JPEG_320X240_50FPS=y" not in defaults
    active_resolution_lines = [
        line for line in defaults.splitlines()
        if line.startswith("CONFIG_CAMERA_OV2640_DVP_") and line.endswith("=y")
    ]
    assert active_resolution_lines == ["CONFIG_CAMERA_OV2640_DVP_YUV422_240X240_25FPS=y"]

def test_firmware_camera_service_uses_snapshot_only_quality() -> None:
    root = Path(__file__).resolve().parents[2] / "firmware" / "main-controller"
    camera_h = (
        root
        / "components"
        / "services"
        / "camera_service"
        / "camera_service.h"
    ).read_text(encoding="utf-8")
    camera_c = (
        root
        / "components"
        / "services"
        / "camera_service"
        / "camera_service.c"
    ).read_text(encoding="utf-8")

    # Camera is no longer a monitoring/video feature. It is used for short
    # vision captures, so the service keeps one high-detail snapshot quality
    # path and must not expose live-stream quality/downscale knobs.
    assert "nb_camera_quality_t" in camera_h
    assert "NB_CAMERA_QUALITY_SNAPSHOT" in camera_h
    assert "NB_CAMERA_QUALITY_LIVE" not in camera_h
    assert (
        "esp_err_t camera_service_capture_snapshot(nb_camera_snapshot_t *out,\n"
        "                                          nb_camera_quality_t quality);"
        in camera_h
    )
    assert "#define CAMERA_SVC_SNAPSHOT_JPEG_QUALITY 82" in camera_c
    assert "CAMERA_SVC_LIVE" not in camera_c
    assert "downscale_yuv422_for_live" not in camera_c
    assert "camera_service_live_target_size" not in camera_c
    assert "camera_service_is_stream_active" not in camera_h
    assert "camera_service_set_stream_active" not in camera_h
    assert ".quality = quality" in camera_c

def test_firmware_camera_service_exposes_no_jpeg_scene_observation() -> None:
    root = Path(__file__).resolve().parents[2] / "firmware" / "main-controller"
    camera_h = (
        root
        / "components"
        / "services"
        / "camera_service"
        / "camera_service.h"
    ).read_text(encoding="utf-8")
    camera_c = (
        root
        / "components"
        / "services"
        / "camera_service"
        / "camera_service.c"
    ).read_text(encoding="utf-8")

    assert "nb_camera_observation_t" in camera_h
    assert "camera_service_observe_scene" in camera_h
    assert "sem gerar JPEG" in camera_h
    assert "esp_err_t camera_service_observe_scene" in camera_c
    assert "camera_service_analyze_yuv422(frame)" in camera_c
    assert "s_last_jpeg_bytes = 0U" in camera_c
    assert "camera_service_hold_arm(CAMERA_SVC_SESSION_HOLD_US)" in camera_c

def test_firmware_camera_status_reports_last_real_frame() -> None:
    root = Path(__file__).resolve().parents[2] / "firmware" / "main-controller"
    camera_h = (
        root
        / "components"
        / "services"
        / "camera_service"
        / "camera_service.h"
    ).read_text(encoding="utf-8")
    camera_c = (
        root
        / "components"
        / "services"
        / "camera_service"
        / "camera_service.c"
    ).read_text(encoding="utf-8")
    web_c = (root / "components" / "infra" / "web_service.c").read_text(
        encoding="utf-8"
    )

    assert "last_frame_bytes" in camera_h
    assert "last_frame_width" in camera_h
    assert "last_frame_height" in camera_h
    assert "last_frame_format" in camera_h
    assert "effective_width" in camera_h
    assert "effective_height" in camera_h
    assert "last_sfmt_errno" in camera_h
    assert "camera_service_format_name" in camera_h
    assert 'case V4L2_PIX_FMT_YUYV:   return "yuyv";' in camera_c
    assert 'case V4L2_PIX_FMT_YUV422P: return "yuv422p";' in camera_c
    assert "s_last_frame_width = frame->width" in camera_c
    assert "out->effective_width = camera_hal_effective_width();" in camera_c
    assert "out->last_sfmt_errno = camera_hal_last_sfmt_errno();" in camera_c
    assert "has_last_frame ? diag.last_frame_width : diag.mode_width" in web_c
    assert '\\"last_frame_format\\":\\"%s\\",' in web_c
    assert '\\"format\\":\\"%s\\",\\"width\\":%lu,\\"height\\":%lu' in web_c
    assert '"format":"jpeg","width":%lu,"height":%lu' not in web_c
    assert '\\"effective_width\\":%lu,\\"effective_height\\":%lu' in web_c
    assert '\\"last_sfmt_errno\\":%d}' in web_c
    assert "stream_active" not in web_c

def test_server_and_app_do_not_expose_camera_monitoring_stream() -> None:
    root = Path(__file__).resolve().parents[2]
    http_py = (
        root
        / "server"
        / "noisebot_server"
        / "internal"
        / "ops"
        / "http.py"
    ).read_text(encoding="utf-8")
    contract_py = (root / "server" / "noisebot_server" / "api" / "contract.py").read_text(
        encoding="utf-8"
    )
    app_api = (root / "app" / "src" / "api.ts").read_text(encoding="utf-8")
    app_tsx = (root / "app" / "src" / "App.tsx").read_text(encoding="utf-8")

    for text in (http_py, contract_py, app_api, app_tsx):
        assert "stream.mjpg" not in text
        assert "visionStreamUrl" not in text
        assert "Monitoramento ao vivo" not in text

def test_firmware_camera_stream_endpoint_is_removed() -> None:
    root = Path(__file__).resolve().parents[2] / "firmware" / "main-controller"
    camera_h = (
        root
        / "components"
        / "services"
        / "camera_service"
        / "camera_service.h"
    ).read_text(encoding="utf-8")
    web_c = (root / "components" / "infra" / "web_service.c").read_text(encoding="utf-8")

    assert '"/api/camera/stream.mjpg"' not in web_c
    assert "handle_api_camera_stream" not in web_c
    assert "mjpeg_stream_task" not in web_c
    assert "s_mjpeg_active" not in web_c
    assert "stream_active" not in web_c
    assert "camera_service_is_stream_active" not in camera_h
    assert "camera_service_set_stream_active" not in camera_h

def test_firmware_idle_suppresses_pose_tilt_while_camera_active() -> None:
    root = Path(__file__).resolve().parents[2] / "firmware" / "main-controller"
    idle_h = (
        root
        / "components"
        / "services"
        / "idle_service"
        / "idle_service.h"
    ).read_text(encoding="utf-8")
    idle_c = (
        root
        / "components"
        / "services"
        / "idle_service"
        / "idle_service.c"
    ).read_text(encoding="utf-8")
    boot_c = (root / "components" / "infra" / "boot_manager.c").read_text(
        encoding="utf-8"
    )

    assert "idle_service_set_camera_active" in idle_h
    assert "static volatile bool s_camera_active = false;" in idle_c
    assert "IDLE_MOTIF_CAMERA_STEADY" in idle_c
    assert "if (s_camera_active)" in idle_c
    assert "s_motif = IDLE_MOTIF_CAMERA_STEADY;" in idle_c
    assert "gaze_service_set_target(0.0f, 0.0f);" in idle_c
    assert "expression_service_set_idle_rotation(0.0f, 0.0f);" in idle_c
    assert "idle_service_set_camera_active(true);" in boot_c
    assert "idle_service_set_camera_active(false);" in boot_c

def test_firmware_camera_active_overlay_contract_is_exposed() -> None:
    root = Path(__file__).resolve().parents[2] / "firmware" / "main-controller"
    camera_h = (
        root
        / "components"
        / "services"
        / "camera_service"
        / "camera_service.h"
    ).read_text(encoding="utf-8")
    camera_c = (
        root
        / "components"
        / "services"
        / "camera_service"
        / "camera_service.c"
    ).read_text(encoding="utf-8")
    overlay_h = (
        root
        / "components"
        / "services"
        / "ui_overlay_service"
        / "ui_overlay_service.h"
    ).read_text(encoding="utf-8")
    overlay_c = (
        root
        / "components"
        / "services"
        / "ui_overlay_service"
        / "ui_overlay_service.cpp"
    ).read_text(encoding="utf-8")
    boot_c = (root / "components" / "infra" / "boot_manager.c").read_text(encoding="utf-8")

    assert "NB_CAMERA_EVT_SESSION_ACTIVE" in camera_h
    assert "camera_service_set_event_cb" in camera_h
    assert "camera_service_set_session_active(true)" in camera_c
    assert "camera_service_set_session_active(false)" in camera_c
    assert "ui_overlay_camera_set" in overlay_h
    assert "ui_overlay_status_icon_set(NB_UI_STATUS_ICON_CAMERA_ACTIVE, enabled)" in overlay_c
    assert 'icons/generated/nb_ui_overlay_icons.h' in overlay_c
    assert "case NB_UI_STATUS_ICON_CAMERA_ACTIVE:     return &NB_UI_OVERLAY_ICON_CAMERA;" in overlay_c
    assert "draw_icon_mask(spr, asset" in overlay_c
    assert "camera_service_set_event_cb(on_camera_event)" in boot_c
    # ui_overlay_camera_set não é mais chamado em boot_manager — ícone desacoplado

def test_server_vision_soak_collects_stable_samples(monkeypatch) -> None:
    soak = importlib.import_module("noisebot_server.internal.ops.vision_soak")

    calls: list[str] = []
    uptimes = iter([10, 15])

    def fake_get_json(base_url: str, path: str, timeout_s: float) -> dict:
        calls.append(path)
        if path == "api/vision/observe":
            return {
                "ok": True,
                "observation": {
                    "valid": True,
                    "width": 640,
                    "height": 480,
                    "jpeg_bytes": 70000,
                    "capture_ms": 900,
                },
                "presence": {
                    "state": "absent",
                    "score": 40,
                },
            }
        if path == "api/diag":
            return {
                "uptime_s": next(uptimes),
                "fps": 25.1,
                "memory": {"psram_free": 7_000_000},
            }
        if path == "api/camera/status":
            return {
                "heap_dma_free": 18_000,
                "ready": False,
                "active": False,
            }
        if path == "api/render/status":
            return {"fps": 30.5}
        raise AssertionError(path)

    def fake_post_json(
        base_url: str,
        path: str,
        timeout_s: float,
        payload: dict | None = None,
    ) -> dict:
        calls.append(path)
        assert path == "api/camera/session/close"
        return {"ok": True}

    ticks = iter([0.0, 0.0, 0.0, 1.0, 1.0])
    monkeypatch.setattr(soak, "_get_json", fake_get_json)
    monkeypatch.setattr(soak, "_post_json", fake_post_json)

    result = soak.run_vision_soak(
        firmware_url="http://192.168.1.30",
        duration_s=1.0,
        interval_s=1.0,
        expect_absence=True,
        min_fps_required=25.0,
        now_fn=lambda: next(ticks),
        sleep_fn=lambda _: None,
    )

    assert result.ok is True
    assert result.samples == 2
    assert result.valid_observations == 2
    assert result.reboots == 0
    assert result.min_fps == 30.5
    assert result.presence_false_positive_count == 0
    assert result.max_presence_score == 40
    assert result.final_camera_active is False
    assert calls.count("api/vision/observe") == 2
    assert calls.count("api/render/status") == 2

def test_server_vision_soak_can_probe_audio_io_during_observation(monkeypatch) -> None:
    soak = importlib.import_module("noisebot_server.internal.ops.vision_soak")

    calls: list[tuple[str, dict | None]] = []
    probe_running = True

    def fake_get_json(base_url: str, path: str, timeout_s: float) -> dict:
        calls.append((path, None))
        if path == "api/vision/observe":
            return {
                "ok": True,
                "observation": {
                    "valid": True,
                    "jpeg_bytes": 0,
                    "capture_ms": 170,
                },
                "presence": {"state": "absent", "score": 42},
            }
        if path == "api/diag":
            return {"uptime_s": 10, "memory": {"psram_free": 7_000_000}}
        if path == "api/camera/status":
            return {"heap_dma_free": 16_000, "ready": False, "active": False}
        if path == "api/render/status":
            return {"fps": 34.0}
        if path == "api/audio/io-v2":
            return {
                "probe_running": probe_running,
                "i2s_recoveries": 0,
                "dropped_frames": 0,
                "last_error": "ESP_OK",
            }
        raise AssertionError(path)

    def fake_post_json(
        base_url: str,
        path: str,
        timeout_s: float,
        payload: dict | None = None,
    ) -> dict:
        nonlocal probe_running
        calls.append((path, payload))
        if path == "api/audio/io-v2/probe":
            assert payload == {"duration_ms": 5000}
            return {"ok": True}
        if path == "api/audio/io-v2/probe/stop":
            probe_running = False
            return {"ok": True}
        if path == "api/camera/session/close":
            return {"ok": True}
        raise AssertionError(path)

    ticks = iter([0.0, 0.0, 0.0, 0.1, 0.1])
    monkeypatch.setattr(soak, "_get_json", fake_get_json)
    monkeypatch.setattr(soak, "_post_json", fake_post_json)

    result = soak.run_vision_soak(
        firmware_url="http://192.168.1.30",
        duration_s=0.1,
        interval_s=1.0,
        min_fps_required=30.0,
        audio_io_probe_ms=5000,
        now_fn=lambda: next(ticks),
        sleep_fn=lambda _: None,
    )

    assert result.ok is True
    assert result.audio_probe_ms == 5000
    assert result.audio_probe_samples == 2
    assert result.audio_probe_busy_skips == 0
    assert result.audio_probe_failures == 0
    assert result.max_audio_i2s_recoveries == 0
    assert result.max_audio_dropped_frames == 0
    assert result.final_audio_probe_running is False
    assert ("api/audio/io-v2/probe", {"duration_ms": 5000}) in calls
    assert ("api/audio/io-v2/probe/stop", None) in calls

def test_server_vision_presence_trial_prefights_initial_state(monkeypatch) -> None:
    trial = importlib.import_module("noisebot_server.internal.ops.vision_presence_trial")

    calls: list[str] = []
    observations = iter([
        {
            "state": "absent",
            "score": 40,
            "transition_count": 0,
            "detected_event_count": 0,
            "capture_ms": 170,
            "spatial_score": 12,
        },
        {
            "state": "candidate",
            "score": 62,
            "transition_count": 0,
            "detected_event_count": 0,
            "capture_ms": 150,
            "spatial_score": 90,
        },
        {
            "state": "present",
            "score": 42,
            "transition_count": 1,
            "detected_event_count": 1,
            "capture_ms": 148,
            "spatial_score": 88,
        },
    ])

    def fake_get_json(base_url: str, path: str, timeout_s: float) -> dict:
        calls.append(path)
        if path == "api/vision/observe":
            item = next(observations)
            return {
                "ok": True,
                "observation": {
                    "valid": True,
                    "capture_ms": item["capture_ms"],
                    "spatial_score": item["spatial_score"],
                },
                "presence": {
                    "state": item["state"],
                    "score": item["score"],
                    "transition_count": item["transition_count"],
                    "detected_event_count": item["detected_event_count"],
                    "lost_event_count": 0,
                },
            }
        if path == "api/render/status":
            return {"fps": 34.0}
        raise AssertionError(path)

    def fake_post_json(base_url: str, path: str, timeout_s: float) -> dict:
        calls.append(path)
        if path == "api/vision/presence/reset":
            return {"ok": True}
        if path == "api/camera/session/close":
            return {"ok": True}
        raise AssertionError(path)

    ticks = iter([0.0, 1.0, 1.1, 1.2, 1.5, 1.8])
    sleeps: list[float] = []
    monkeypatch.setattr(trial, "_get_json", fake_get_json)
    monkeypatch.setattr(trial, "_post_json", fake_post_json)

    result = trial.run_vision_presence_trial(
        firmware_url="http://192.168.1.30",
        mode="presence",
        duration_s=0.4,
        interval_s=0.3,
        reset_presence=True,
        start_delay_s=1.0,
        require_initial_state="absent",
        require_final_state="present",
        min_fps_required=25.0,
        max_latency_ms=1000.0,
        now_fn=lambda: next(ticks),
        sleep_fn=lambda value: sleeps.append(value),
    )

    assert result.ok is True
    assert result.initial_presence_state == "absent"
    assert result.final_presence_state == "present"
    assert result.detected_event_delta == 1
    assert result.min_capture_ms == 148
    assert result.max_capture_ms == 170
    assert result.p95_capture_ms == 170
    assert result.max_spatial_score == 90
    assert sleeps[0] == 1.0
    assert calls[:3] == [
        "api/vision/presence/reset",
        "api/render/status",
        "api/vision/observe",
    ]
    assert "api/camera/session/close" in calls

def test_server_cli_runs_debug_vision_soak(monkeypatch) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    soak = importlib.import_module("noisebot_server.internal.ops.vision_soak")

    calls: dict[str, object] = {}

    def fake_run_vision_soak(
        *,
        firmware_url: str,
        duration_s: float,
        interval_s: float,
        timeout_s: float,
        expect_absence: bool,
        min_fps_required: float | None,
        audio_io_probe_ms: int,
    ):
        calls["firmware_url"] = firmware_url
        calls["duration_s"] = duration_s
        calls["interval_s"] = interval_s
        calls["timeout_s"] = timeout_s
        calls["expect_absence"] = expect_absence
        calls["min_fps_required"] = min_fps_required
        calls["audio_io_probe_ms"] = audio_io_probe_ms
        return soak.VisionSoakResult(
            ok=True,
            duration_s=duration_s,
            interval_s=interval_s,
            samples=1,
            failures=0,
            reboots=0,
            valid_observations=1,
            first_uptime_s=10,
            last_uptime_s=10,
            min_fps=25.0,
            min_psram_free=7_000_000,
            min_dma_free=18_000,
            max_capture_ms=900,
            max_jpeg_bytes=70_000,
            presence_present_samples=0,
            presence_candidate_samples=0,
            presence_false_positive_count=0,
            max_presence_score=40,
            final_presence_state="absent",
            min_fps_required=min_fps_required,
            expect_absence=expect_absence,
            final_camera_ready=False,
            final_camera_active=False,
            final_close_ok=True,
            worst_fps_sample={"sample": 1, "fps": 25.0},
            audio_probe_ms=audio_io_probe_ms,
            audio_probe_samples=1,
            audio_probe_busy_skips=0,
            audio_probe_failures=0,
            max_audio_i2s_recoveries=0,
            max_audio_dropped_frames=0,
            final_audio_probe_running=False,
            final_audio_last_error="ESP_OK",
            errors=[],
        )

    monkeypatch.setattr(soak, "run_vision_soak", fake_run_vision_soak)

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "vision-soak",
        "--duration-s",
        "3",
        "--interval-s",
        "1",
        "--timeout-s",
        "2",
        "--expect-absence",
        "--min-fps",
        "25",
        "--audio-io-probe-ms",
        "5000",
        "--json",
    ])

    assert calls == {
        "firmware_url": "http://192.168.1.30",
        "duration_s": 3.0,
        "interval_s": 1.0,
        "timeout_s": 2.0,
        "expect_absence": True,
        "min_fps_required": 25.0,
        "audio_io_probe_ms": 5000,
    }

def test_validate_arguments_analyze_vision_no_required_args() -> None:
    catalog = importlib.import_module("noisebot_server.internal.agent.tools.catalog")
    spec = catalog.CATALOG["analyze_vision"]

    errors = catalog.validate_arguments(spec, {})

    assert errors == []

def test_executor_analyze_vision_without_client_returns_error() -> None:
    executors = importlib.import_module("noisebot_server.internal.agent.tools.executors")

    result = executors.execute_analyze_vision({}, {"vision_client": None, "turn_id": 1})

    assert "error" in result
    assert "indisponivel" in result["error"]

def test_gateway_analyze_vision_vetoed_when_unavailable() -> None:
    gateway = importlib.import_module("noisebot_server.internal.agent.tools.gateway")

    result = gateway.execute_tool_call(
        {"name": "analyze_vision", "arguments": {}},
        vision_available=False,
        turn_id=1,
    )

    assert result.vetoed is True
    assert "visao" in (result.error or "").lower()

def test_gateway_analyze_vision_executes_when_available() -> None:
    gateway = importlib.import_module("noisebot_server.internal.agent.tools.gateway")

    result = gateway.execute_tool_call(
        {"name": "analyze_vision", "arguments": {}},
        vision_available=True,
        vision_client=None,  # sem cliente real: executor retorna error dict, mas gateway não veta
        turn_id=1,
    )

    # Gateway não veta — executor roda e devolve erro interno
    assert result.vetoed is False
    assert result.success is True  # executor retornou dict com "error" mas não levantou
    assert "error" in result.result

def test_local_vision_scene_uses_ollama_vision_description(monkeypatch) -> None:
    import unittest.mock as mock

    agent = importlib.import_module("noisebot_server.internal.agent")
    intents = importlib.import_module("noisebot_server.internal.agent.intents")
    vision = importlib.import_module("noisebot_server.internal.vision")

    obs = _make_fake_vision_obs(luma=120, contrast=120)
    analysis = vision.VisionAnalysis(
        observation=obs,
        detector="yunet",
        detector_available=True,
        face_detected=False,
        face_count=0,
        primary_face=None,
    )

    class FakeVisionClient:
        def observe(self):
            return obs

        def snapshot(self):
            return b"\xff\xd8\xff"

        def session_close(self):
            pass

    monkeypatch.setenv("NOISEBOT_LLM_PROVIDER", "ollama")
    monkeypatch.setenv("NOISEBOT_LLM_MODEL", "gemma4:12b")
    monkeypatch.setenv("NOISEBOT_OLLAMA_BASE_URL", "http://127.0.0.1:11434")
    monkeypatch.setattr(intents, "analyze_jpeg", mock.Mock(return_value=analysis))
    monkeypatch.setattr(
        intents,
        "describe_with_ollama_vision",
        mock.Mock(return_value="Vejo voce sentado em frente ao robo."),
    )
    monkeypatch.setattr(intents, "describe_with_vision_api", mock.Mock(return_value=None))

    provider = agent.LocalIntentProvider(vision_client=FakeVisionClient())
    result = provider.match("o que voce esta vendo?", turn_id=12)

    assert result.intent_name == "local_vision_scene"
    assert result.reply_text == "Vejo voce sentado em frente ao robo."
    intents.describe_with_ollama_vision.assert_called_once_with(
        b"\xff\xd8\xff",
        base_url="http://127.0.0.1:11434",
        model="gemma4:12b",
    )
    intents.describe_with_vision_api.assert_not_called()

def test_local_vision_scene_falls_back_to_local_reading(monkeypatch) -> None:
    import unittest.mock as mock

    agent = importlib.import_module("noisebot_server.internal.agent")
    intents = importlib.import_module("noisebot_server.internal.agent.intents")
    vision = importlib.import_module("noisebot_server.internal.vision")

    obs = _make_fake_vision_obs(motion=0, luma=120, contrast=120)
    face = vision.FaceBox(x=96, y=72, width=48, height=64)
    analysis = vision.VisionAnalysis(
        observation=obs,
        detector="yunet",
        detector_available=True,
        face_detected=True,
        face_count=1,
        primary_face=face,
    )

    class FakeVisionClient:
        def observe(self):
            return obs

        def snapshot(self):
            return b"\xff\xd8\xff"

        def session_close(self):
            pass

    monkeypatch.delenv("OPENAI_API_KEY", raising=False)
    monkeypatch.setenv("NOISEBOT_LLM_PROVIDER", "ollama")
    monkeypatch.setattr(intents, "analyze_jpeg", mock.Mock(return_value=analysis))
    monkeypatch.setattr(intents, "describe_with_ollama_vision", mock.Mock(return_value=None))
    monkeypatch.setattr(intents, "describe_with_vision_api", mock.Mock(return_value=None))

    provider = agent.LocalIntentProvider(vision_client=FakeVisionClient())
    result = provider.match("descreva a cena", turn_id=13)

    assert result.intent_name == "local_vision_scene"
    assert "detectei um rosto" in result.reply_text
    assert "quase nao percebi movimento" in result.reply_text

def test_payload_vision_field_present_when_snapshot_provided() -> None:
    """build_turn_payload inclui 'vision' quando vision_snapshot é fornecido."""
    payload_builder = importlib.import_module(
        "noisebot_server.internal.agent.payload_builder"
    )

    snapshot = {
        "available": True,
        "fresh": True,
        "scene": "usuario na mesa",
        "brightness": "média",
        "motion": "baixo",
    }
    payload = payload_builder.build_turn_payload(
        text="olá",
        turn_id=1,
        vision_snapshot=snapshot,
    )

    assert "vision" in payload
    assert payload["vision"]["scene"] == "usuario na mesa"
    assert payload["vision"]["brightness"] == "média"
    assert payload["vision"]["motion"] == "baixo"

def test_payload_vision_absent_when_snapshot_none() -> None:
    """build_turn_payload não inclui 'vision' quando vision_snapshot é None."""
    payload_builder = importlib.import_module(
        "noisebot_server.internal.agent.payload_builder"
    )

    payload = payload_builder.build_turn_payload(
        text="olá",
        turn_id=1,
        vision_snapshot=None,
    )

    assert "vision" not in payload

def test_payload_vision_strips_bytes_values() -> None:
    """vision_snapshot não pode vazar bytes/bytearray no payload."""
    payload_builder = importlib.import_module(
        "noisebot_server.internal.agent.payload_builder"
    )

    snapshot = {
        "available": True,
        "scene": "sala",
        "jpeg_raw": b"\xff\xd8\xff",
        "frame_data": bytearray(b"\x00\x01"),
        "brightness": "alta",
    }
    payload = payload_builder.build_turn_payload(
        text="o que você está vendo?",
        turn_id=2,
        vision_snapshot=snapshot,
    )

    vision = payload.get("vision", {})
    assert "jpeg_raw" not in vision
    assert "frame_data" not in vision
    assert vision.get("brightness") == "alta"
    assert vision.get("scene") == "sala"

def test_payload_vision_has_text_fields_not_ints() -> None:
    """brightness e motion devem ser strings textuais, não inteiros."""
    payload_builder = importlib.import_module(
        "noisebot_server.internal.agent.payload_builder"
    )

    snapshot = {
        "available": True,
        "scene": "quarto escuro",
        "brightness": "baixa",
        "motion": "moderado",
    }
    payload = payload_builder.build_turn_payload(
        text="test",
        turn_id=3,
        vision_snapshot=snapshot,
    )

    vision = payload["vision"]
    assert isinstance(vision["brightness"], str)
    assert isinstance(vision["motion"], str)
    assert vision["brightness"] in ("baixa", "média", "alta")
    assert vision["motion"] in ("baixo", "moderado", "alto")

def test_vision_client_has_get_lightweight_snapshot() -> None:
    """VisionClient deve ter o método get_lightweight_snapshot."""
    vision_module = importlib.import_module(
        "noisebot_server.internal.vision.client"
    )
    client = vision_module.VisionClient("http://localhost:8080")
    assert callable(getattr(client, "get_lightweight_snapshot", None))

def test_vision_lightweight_snapshot_returns_none_on_error() -> None:
    """get_lightweight_snapshot retorna None (nunca levanta) quando observe() falha."""
    vision_module = importlib.import_module(
        "noisebot_server.internal.vision.client"
    )
    client = vision_module.VisionClient("http://127.0.0.1:1", timeout_s=0.01)

    result = client.get_lightweight_snapshot()

    assert result is None

def test_vision_lightweight_snapshot_structure() -> None:
    """get_lightweight_snapshot retorna dict com campos obrigatórios quando observe() ok."""
    import unittest.mock as mock

    vision_module = importlib.import_module(
        "noisebot_server.internal.vision.client"
    )
    client = vision_module.VisionClient("http://localhost:8080")

    fake_obs = vision_module.VisionObservation(
        valid=True,
        scene="frente do usuario",
        timestamp_ms=12345,
        width=320,
        height=240,
        jpeg_bytes=0,
        capture_ms=10,
        luma_avg=80,
        luma_min=30,
        luma_max=200,
        contrast=50,
        motion_score=5,
    )

    with mock.patch.object(client, "observe", return_value=fake_obs):
        result = client.get_lightweight_snapshot()

    assert result is not None
    assert result["available"] is True
    assert result["fresh"] is True
    assert result["scene"] == "frente do usuario"
    assert result["brightness"] == "média"   # luma_avg=80 → 60-150 range
    assert result["motion"] == "baixo"       # motion_score=5 < 10

def test_format_turn_payload_includes_vision_section() -> None:
    """_format_turn_payload_block deve incluir linha de visão quando presente."""
    llm_module = importlib.import_module("noisebot_server.internal.agent.llm")

    payload = {
        "turn": {"id": 1, "user_text": "test", "timestamp_iso": "2026-01-01T00:00:00+00:00"},
        "robot": {"state": "IDLE", "firmware_online": False, "pipeline_mode": "normal"},
        "mood": "neutro",
        "vision": {
            "available": True,
            "fresh": True,
            "scene": "mesa de trabalho",
            "brightness": "alta",
            "motion": "baixo",
        },
    }

    block = llm_module._format_turn_payload_block(payload)

    assert "Visao:" in block or "Visao" in block
    assert "mesa de trabalho" in block
    assert "alta" in block

def test_format_turn_payload_no_vision_section_when_absent() -> None:
    """_format_turn_payload_block não deve incluir linha Visao quando ausente."""
    llm_module = importlib.import_module("noisebot_server.internal.agent.llm")

    payload = {
        "turn": {"id": 1, "user_text": "test", "timestamp_iso": "2026-01-01T00:00:00+00:00"},
        "robot": {"state": "IDLE", "firmware_online": False, "pipeline_mode": "normal"},
        "mood": "neutro",
    }

    block = llm_module._format_turn_payload_block(payload)

    assert "Visao" not in block

def test_vision_client_luma_to_text_thresholds() -> None:
    """_luma_to_text mapeia corretamente os limites de brilho."""
    vision_module = importlib.import_module(
        "noisebot_server.internal.vision.client"
    )
    _luma_to_text = vision_module._luma_to_text

    assert _luma_to_text(0) == "baixa"
    assert _luma_to_text(59) == "baixa"
    assert _luma_to_text(60) == "média"
    assert _luma_to_text(149) == "média"
    assert _luma_to_text(150) == "alta"
    assert _luma_to_text(255) == "alta"

def test_vision_client_motion_to_text_thresholds() -> None:
    """_motion_to_text mapeia corretamente os limites de movimento."""
    vision_module = importlib.import_module(
        "noisebot_server.internal.vision.client"
    )
    _motion_to_text = vision_module._motion_to_text

    assert _motion_to_text(0) == "baixo"
    assert _motion_to_text(9) == "baixo"
    assert _motion_to_text(10) == "moderado"
    assert _motion_to_text(49) == "moderado"
    assert _motion_to_text(50) == "alto"
    assert _motion_to_text(100) == "alto"


# ── YuNet analyzer ────────────────────────────────────────────────────────────

def test_analyzer_returns_unavailable_when_not_initialized() -> None:
    """analyze_jpeg retorna detector_available=False se init_analyzer não foi chamado."""
    import unittest.mock as mock
    analysis_module = importlib.import_module("noisebot_server.internal.vision.analysis")

    with mock.patch.object(analysis_module, "_detector", None):
        obs = importlib.import_module("noisebot_server.internal.vision.client").VisionObservation(
            valid=True, scene="normal", timestamp_ms=0, width=240, height=240,
            jpeg_bytes=0, capture_ms=0, luma_avg=0, luma_min=0, luma_max=0,
            contrast=0, motion_score=0,
        )
        result = analysis_module.analyze_jpeg(b"\xff\xd8\xff", obs)

    assert result.detector_available is False
    assert result.face_detected is False
    assert result.error == "detector_not_initialized"
    assert result.detector == "yunet"


def test_analyzer_is_detector_available_reflects_singleton() -> None:
    import unittest.mock as mock
    analysis_module = importlib.import_module("noisebot_server.internal.vision.analysis")

    with mock.patch.object(analysis_module, "_detector", None):
        assert analysis_module.is_detector_available() is False

    sentinel = object()
    with mock.patch.object(analysis_module, "_detector", sentinel):
        assert analysis_module.is_detector_available() is True


def test_init_analyzer_raises_when_model_missing(tmp_path) -> None:
    """init_analyzer levanta RuntimeError com mensagem clara quando o modelo não existe."""
    import unittest.mock as mock
    analysis_module = importlib.import_module("noisebot_server.internal.vision.analysis")

    # Patch cv2 import to succeed but point at a nonexistent model path
    fake_cv2 = mock.MagicMock()
    with mock.patch.dict("sys.modules", {"cv2": fake_cv2}):
        with pytest.raises(RuntimeError, match="Modelo YuNet"):
            analysis_module.init_analyzer(model_path=tmp_path / "nonexistent.onnx")


def test_init_analyzer_raises_when_opencv_missing() -> None:
    """init_analyzer levanta RuntimeError com instrução pip quando cv2 não está disponível."""
    analysis_module = importlib.import_module("noisebot_server.internal.vision.analysis")

    import sys
    original = sys.modules.get("cv2")
    sys.modules["cv2"] = None  # type: ignore[assignment]
    try:
        with pytest.raises(RuntimeError, match="pip install"):
            analysis_module.init_analyzer()
    finally:
        if original is None:
            sys.modules.pop("cv2", None)
        else:
            sys.modules["cv2"] = original


def test_describe_with_ollama_vision_posts_image_payload(monkeypatch) -> None:
    import base64
    import json

    analysis_module = importlib.import_module("noisebot_server.internal.vision.analysis")
    captured: dict[str, object] = {}

    class FakeResponse:
        def __enter__(self):
            return self

        def __exit__(self, *_):
            return False

        def read(self):
            return b'{"message":{"content":"Vejo uma pessoa na mesa."}}'

    def fake_urlopen(req, timeout):
        captured["url"] = req.full_url
        captured["timeout"] = timeout
        captured["payload"] = json.loads(req.data.decode("utf-8"))
        return FakeResponse()

    monkeypatch.setattr("urllib.request.urlopen", fake_urlopen)

    result = analysis_module.describe_with_ollama_vision(
        b"jpeg-bytes",
        base_url="http://127.0.0.1:11434",
        model="gemma4:12b",
    )

    assert result == "Vejo uma pessoa na mesa."
    assert captured["url"] == "http://127.0.0.1:11434/api/chat"
    payload = captured["payload"]
    assert payload["model"] == "gemma4:12b"
    assert payload["stream"] is False
    assert payload["think"] is False
    assert payload["messages"][0]["images"] == [
        base64.b64encode(b"jpeg-bytes").decode("ascii")
    ]


def test_describe_with_ollama_vision_falls_back_to_generate(monkeypatch) -> None:
    import json

    analysis_module = importlib.import_module("noisebot_server.internal.vision.analysis")
    calls: list[tuple[str, dict]] = []

    class FakeResponse:
        def __init__(self, payload: bytes) -> None:
            self._payload = payload

        def __enter__(self):
            return self

        def __exit__(self, *_):
            return False

        def read(self):
            return self._payload

    def fake_urlopen(req, timeout):
        payload = json.loads(req.data.decode("utf-8"))
        calls.append((req.full_url, payload))
        if req.full_url.endswith("/api/chat"):
            return FakeResponse(b'{"message":{"content":""}}')
        return FakeResponse(b'{"response":"Vejo um ambiente interno com uma pessoa."}')

    monkeypatch.setattr("urllib.request.urlopen", fake_urlopen)

    result = analysis_module.describe_with_ollama_vision(
        b"jpeg-bytes",
        base_url="http://127.0.0.1:11434",
        model="gemma4:12b",
    )

    assert result == "Vejo um ambiente interno com uma pessoa."
    assert calls[0][0] == "http://127.0.0.1:11434/api/chat"
    assert calls[1][0] == "http://127.0.0.1:11434/api/generate"
    assert calls[0][1]["think"] is False
    assert calls[1][1]["think"] is False
    assert calls[1][1]["images"]


def test_analyzer_low_light_fallback_runs_when_first_detection_misses() -> None:
    """Em baixa luz, analyze_jpeg tenta uma segunda detecção no frame realçado."""
    import unittest.mock as mock
    analysis_module = importlib.import_module("noisebot_server.internal.vision.analysis")
    obs = _make_fake_vision_obs(luma=35)

    fake_image = mock.MagicMock()
    fake_image.shape = (240, 240, 3)
    fake_cv2 = mock.MagicMock()
    fake_cv2.IMREAD_COLOR = 1
    fake_cv2.imdecode.return_value = fake_image
    fake_np = mock.MagicMock()
    fake_np.uint8 = object()
    fake_np.frombuffer.return_value = b"arr"
    fake_detector = mock.MagicMock()
    fake_detector.detect.side_effect = [
        (None, None),
        (None, [[80, 60, 50, 70, 0.8]]),
    ]

    with mock.patch.dict("sys.modules", {"cv2": fake_cv2, "numpy": fake_np}), \
         mock.patch.object(analysis_module, "_detector", fake_detector), \
         mock.patch.object(analysis_module, "_enhance_low_light_image", return_value=fake_image):
        result = analysis_module.analyze_jpeg(b"\xff\xd8\xff", obs)

    assert fake_detector.detect.call_count == 2
    assert result.face_detected is True
    assert result.primary_face is not None
    assert result.primary_face.width == 50


def test_analyzer_skips_low_light_fallback_in_good_light() -> None:
    """Em luz normal, um miss simples não deve pagar custo extra de realce."""
    import unittest.mock as mock
    analysis_module = importlib.import_module("noisebot_server.internal.vision.analysis")
    obs = _make_fake_vision_obs(luma=130, contrast=120)

    fake_image = mock.MagicMock()
    fake_image.shape = (240, 240, 3)
    fake_cv2 = mock.MagicMock()
    fake_cv2.IMREAD_COLOR = 1
    fake_cv2.imdecode.return_value = fake_image
    fake_np = mock.MagicMock()
    fake_np.uint8 = object()
    fake_np.frombuffer.return_value = b"arr"
    fake_detector = mock.MagicMock()
    fake_detector.detect.return_value = (None, None)

    with mock.patch.dict("sys.modules", {"cv2": fake_cv2, "numpy": fake_np}), \
         mock.patch.object(analysis_module, "_detector", fake_detector), \
         mock.patch.object(analysis_module, "_enhance_low_light_image") as enhance:
        result = analysis_module.analyze_jpeg(b"\xff\xd8\xff", obs)

    assert fake_detector.detect.call_count == 1
    enhance.assert_not_called()
    assert result.face_detected is False


# ── VisionPipeline ────────────────────────────────────────────────────────────

def _make_fake_vision_obs(
    motion: int = 0,
    luma: int = 0,
    width: int = 240,
    height: int = 240,
    contrast: int = 30,
):
    VisionObservation = importlib.import_module(
        "noisebot_server.internal.vision.client"
    ).VisionObservation
    return VisionObservation(
        valid=True, scene="normal", timestamp_ms=0,
        width=width, height=height, jpeg_bytes=0, capture_ms=0,
        luma_avg=luma, luma_min=0, luma_max=255, contrast=contrast, motion_score=motion,
    )


def test_vision_pipeline_initial_state_is_idle() -> None:
    pipeline_module = importlib.import_module(
        "noisebot_server.internal.vision.vision_pipeline"
    )
    pipe = pipeline_module.VisionPipeline(
        vision_client=None,  # type: ignore[arg-type]
        get_adapter=lambda: None,
    )
    assert pipe.state == pipeline_module.PipelineState.IDLE


def test_vision_pipeline_status_dict_structure() -> None:
    pipeline_module = importlib.import_module(
        "noisebot_server.internal.vision.vision_pipeline"
    )
    pipe = pipeline_module.VisionPipeline(
        vision_client=None,  # type: ignore[arg-type]
        get_adapter=lambda: None,
    )
    d = pipe.status_dict()
    assert "state" in d
    assert "detector_available" in d
    assert "adapter_connected" in d
    assert "detections" in d
    assert "gaze_sends" in d
    assert "capture_errors" in d
    assert "last_face_box" in d


def test_vision_pipeline_idle_check_interval_is_responsive() -> None:
    """IDLE não deve esperar dezenas de segundos para procurar presença."""
    pipeline_module = importlib.import_module(
        "noisebot_server.internal.vision.vision_pipeline"
    )
    assert pipeline_module._IDLE_CHECK_INTERVAL_S <= 5.0


@pytest.mark.asyncio
async def test_vision_pipeline_idle_stays_idle_without_motion() -> None:
    """IDLE permanece IDLE quando motion_score=0 e luma=0."""
    import unittest.mock as mock
    pipeline_module = importlib.import_module(
        "noisebot_server.internal.vision.vision_pipeline"
    )

    obs = _make_fake_vision_obs(motion=0, luma=0)

    pipe = pipeline_module.VisionPipeline(
        vision_client=None,  # type: ignore[arg-type]
        get_adapter=lambda: None,
    )
    with mock.patch.object(pipe, "_safe_observe", return_value=obs):
        interval = await pipe._tick_idle()

    assert pipe.state == pipeline_module.PipelineState.IDLE
    assert interval == pipeline_module._IDLE_CHECK_INTERVAL_S


@pytest.mark.asyncio
async def test_vision_pipeline_idle_to_acquire_on_motion() -> None:
    """IDLE → ACQUIRE quando motion_score > 10."""
    import unittest.mock as mock
    pipeline_module = importlib.import_module(
        "noisebot_server.internal.vision.vision_pipeline"
    )

    obs = _make_fake_vision_obs(motion=50)
    pipe = pipeline_module.VisionPipeline(
        vision_client=None,  # type: ignore[arg-type]
        get_adapter=lambda: None,
    )
    with mock.patch.object(pipe, "_safe_observe", return_value=obs):
        interval = await pipe._tick_idle()

    assert pipe.state == pipeline_module.PipelineState.ACQUIRE
    assert interval == pipeline_module._ACQUIRE_INTERVAL_S


@pytest.mark.asyncio
async def test_vision_pipeline_idle_to_acquire_on_static_face() -> None:
    """IDLE também acorda quando há rosto estático, mesmo com motion_score baixo."""
    import unittest.mock as mock
    pipeline_module = importlib.import_module(
        "noisebot_server.internal.vision.vision_pipeline"
    )
    analysis_module = importlib.import_module(
        "noisebot_server.internal.vision.analysis"
    )

    obs = _make_fake_vision_obs(motion=0, luma=120)
    face = analysis_module.FaceBox(x=100, y=70, width=56, height=72)
    fake_analysis = analysis_module.VisionAnalysis(
        observation=obs,
        detector="yunet",
        detector_available=True,
        face_detected=True,
        face_count=1,
        primary_face=face,
    )
    pipe = pipeline_module.VisionPipeline(
        vision_client=None,  # type: ignore[arg-type]
        get_adapter=lambda: None,
    )

    with mock.patch.object(pipe, "_safe_observe", return_value=obs), \
         mock.patch.object(pipe, "_safe_capture", return_value=b"\xff\xd8\xff"), \
         mock.patch("noisebot_server.internal.vision.vision_pipeline.analyze_jpeg",
                    return_value=fake_analysis):
        interval = await pipe._tick_idle()

    assert pipe.state == pipeline_module.PipelineState.ACQUIRE
    assert pipe._consecutive_hits == 1
    assert interval == pipeline_module._ACQUIRE_INTERVAL_S


@pytest.mark.asyncio
async def test_vision_pipeline_acquire_to_track_after_hits() -> None:
    """ACQUIRE → TRACK após _CONFIRM_HITS detecções consecutivas."""
    import unittest.mock as mock
    pipeline_module = importlib.import_module(
        "noisebot_server.internal.vision.vision_pipeline"
    )
    analysis_module = importlib.import_module(
        "noisebot_server.internal.vision.analysis"
    )

    obs = _make_fake_vision_obs(motion=50)
    face = analysis_module.FaceBox(x=80, y=60, width=80, height=80)
    fake_analysis = analysis_module.VisionAnalysis(
        observation=obs,
        detector="yunet",
        detector_available=True,
        face_detected=True,
        face_count=1,
        primary_face=face,
    )

    pipe = pipeline_module.VisionPipeline(
        vision_client=None,  # type: ignore[arg-type]
        get_adapter=lambda: None,
    )
    pipe._state = pipeline_module.PipelineState.ACQUIRE
    pipe._cached_obs = obs
    pipe._acquire_start_ts = asyncio.get_event_loop().time()

    with mock.patch.object(pipe, "_safe_capture", return_value=b"\xff\xd8\xff"), \
         mock.patch("noisebot_server.internal.vision.vision_pipeline.analyze_jpeg",
                    return_value=fake_analysis), \
         mock.patch.object(pipe, "_emit_gaze") as emit_gaze:
        # Need _CONFIRM_HITS consecutive hits
        for _ in range(pipeline_module._CONFIRM_HITS):
            await pipe._tick_acquire()

    assert pipe.state == pipeline_module.PipelineState.TRACK
    assert pipe.counters.detections >= 1
    emit_gaze.assert_not_called()


@pytest.mark.asyncio
async def test_vision_pipeline_track_to_lost_after_misses() -> None:
    """TRACK → LOST após _MISS_THRESHOLD misses consecutivos."""
    import unittest.mock as mock
    pipeline_module = importlib.import_module(
        "noisebot_server.internal.vision.vision_pipeline"
    )
    analysis_module = importlib.import_module(
        "noisebot_server.internal.vision.analysis"
    )

    obs = _make_fake_vision_obs(motion=5)
    fake_analysis = analysis_module.VisionAnalysis(
        observation=obs, detector="yunet", detector_available=True,
        face_detected=False, face_count=0, primary_face=None,
    )

    pipe = pipeline_module.VisionPipeline(
        vision_client=None,  # type: ignore[arg-type]
        get_adapter=lambda: None,
    )
    pipe._state = pipeline_module.PipelineState.TRACK
    pipe._cached_obs = obs

    async def _noop_clear(): pass
    with mock.patch.object(pipe, "_safe_capture", return_value=b"\xff\xd8\xff"), \
         mock.patch("noisebot_server.internal.vision.vision_pipeline.analyze_jpeg",
                    return_value=fake_analysis), \
         mock.patch.object(pipe, "_clear_face_box", side_effect=_noop_clear):
        for _ in range(pipeline_module._MISS_THRESHOLD):
            await pipe._tick_track()

    assert pipe.state == pipeline_module.PipelineState.LOST


@pytest.mark.asyncio
async def test_vision_pipeline_track_does_not_stream_gaze() -> None:
    """TRACK mantém presença por face_box sem prender o olhar em tracking contínuo."""
    import unittest.mock as mock
    pipeline_module = importlib.import_module(
        "noisebot_server.internal.vision.vision_pipeline"
    )
    analysis_module = importlib.import_module(
        "noisebot_server.internal.vision.analysis"
    )

    obs = _make_fake_vision_obs(width=240, height=240)
    face = analysis_module.FaceBox(x=96, y=80, width=56, height=72)
    fake_analysis = analysis_module.VisionAnalysis(
        observation=obs,
        detector="yunet",
        detector_available=True,
        face_detected=True,
        face_count=1,
        primary_face=face,
    )

    pipe = pipeline_module.VisionPipeline(
        vision_client=None,  # type: ignore[arg-type]
        get_adapter=lambda: None,
    )
    pipe._state = pipeline_module.PipelineState.TRACK
    pipe._cached_obs = obs

    async def _noop_face_box(_): pass
    with mock.patch.object(pipe, "_safe_capture", return_value=b"\xff\xd8\xff"), \
         mock.patch("noisebot_server.internal.vision.vision_pipeline.analyze_jpeg",
                    return_value=fake_analysis), \
         mock.patch.object(pipe, "_emit_gaze") as emit_gaze, \
         mock.patch.object(pipe, "_emit_face_box", side_effect=_noop_face_box) as emit_face_box:
        interval = await pipe._tick_track()

    emit_gaze.assert_not_called()
    assert emit_face_box.call_count == 1
    assert interval == pipeline_module._TRACK_INTERVAL_S
    assert pipe.state == pipeline_module.PipelineState.TRACK


@pytest.mark.asyncio
async def test_vision_pipeline_lost_reacquires_before_idle() -> None:
    """LOST limpa tracking e entra em ACQUIRE rápido antes da cadência lenta de IDLE."""
    pipeline_module = importlib.import_module(
        "noisebot_server.internal.vision.vision_pipeline"
    )

    pipe = pipeline_module.VisionPipeline(
        vision_client=None,  # type: ignore[arg-type]
        get_adapter=lambda: None,
    )
    pipe._state = pipeline_module.PipelineState.LOST
    pipe._consecutive_hits = 2
    pipe._consecutive_misses = pipeline_module._MISS_THRESHOLD
    pipe._cached_obs = object()  # type: ignore[assignment]
    pipe._ema_x = 0.2
    pipe._ema_y = -0.1
    pipe._last_sent_x = 0.2
    pipe._last_sent_y = -0.1

    interval = await pipe._tick_lost()

    assert pipe.state == pipeline_module.PipelineState.ACQUIRE
    assert interval == pipeline_module._ACQUIRE_INTERVAL_S
    assert pipe._consecutive_hits == 0
    assert pipe._consecutive_misses == 0
    assert pipe._cached_obs is None
    assert pipe._ema_x is None
    assert pipe._last_sent_x is None


def test_vision_pipeline_uses_real_observation_dimensions() -> None:
    """O pipeline passa a observação real para analyze_jpeg (não dimensões hardcoded)."""
    import unittest.mock as mock
    analysis_module = importlib.import_module("noisebot_server.internal.vision.analysis")

    # VGA observation
    obs = _make_fake_vision_obs(width=640, height=480)
    face = analysis_module.FaceBox(x=200, y=100, width=120, height=120)
    va = analysis_module.VisionAnalysis(
        observation=obs, detector="yunet", detector_available=True,
        face_detected=True, face_count=1, primary_face=face,
    )

    # Norm coordinates must use the real width/height, not 240
    assert va.face_center_norm_x == pytest.approx((260.0 / 640.0) * 2.0 - 1.0, abs=1e-6)
    assert va.face_center_norm_y == pytest.approx((160.0 / 480.0) * 2.0 - 1.0, abs=1e-6)


def test_vision_pipeline_pull_adapter_not_injection() -> None:
    """VisionPipeline recebe get_adapter callable e não tem set_adapter (P3 corrigido)."""
    pipeline_module = importlib.import_module(
        "noisebot_server.internal.vision.vision_pipeline"
    )
    pipe = pipeline_module.VisionPipeline(
        vision_client=None,  # type: ignore[arg-type]
        get_adapter=lambda: None,
    )
    assert not hasattr(pipe, "set_adapter"), "set_adapter não deve existir no VisionPipeline"
    assert callable(pipe._get_adapter)


def test_vision_pipeline_gaze_ema_smoothing() -> None:
    """EMA suaviza coordenadas de gaze entre ticks."""
    import unittest.mock as mock
    pipeline_module = importlib.import_module(
        "noisebot_server.internal.vision.vision_pipeline"
    )
    analysis_module = importlib.import_module("noisebot_server.internal.vision.analysis")

    obs = _make_fake_vision_obs(width=240, height=240)
    face = analysis_module.FaceBox(x=96, y=96, width=48, height=48)
    va = analysis_module.VisionAnalysis(
        observation=obs, detector="yunet", detector_available=True,
        face_detected=True, face_count=1, primary_face=face,
    )

    sent = []
    class FakeAdapter:
        is_connected = True
        async def send_gaze(self, x, y): sent.append((x, y))
        async def send_face_box(self, *_): pass

    pipe = pipeline_module.VisionPipeline(
        vision_client=None,  # type: ignore[arg-type]
        get_adapter=lambda: FakeAdapter(),
    )

    async def _run():
        # Bootstrap: first detection seeds EMA
        await pipe._emit_gaze(va)
        first_x, first_y = sent[-1]
        # Second detection with different position
        face2 = analysis_module.FaceBox(x=120, y=120, width=48, height=48)
        va2 = analysis_module.VisionAnalysis(
            observation=obs, detector="yunet", detector_available=True,
            face_detected=True, face_count=1, primary_face=face2,
        )
        await pipe._emit_gaze(va2)
        second_x, second_y = sent[-1]
        # EMA-smoothed value should be between the two raw values
        raw1_x = va.face_center_norm_x
        raw2_x = va2.face_center_norm_x
        assert raw1_x is not None and raw2_x is not None
        assert min(raw1_x, raw2_x) <= second_x <= max(raw1_x, raw2_x)

    asyncio.get_event_loop().run_until_complete(_run())
