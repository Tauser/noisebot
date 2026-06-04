from __future__ import annotations

import importlib


def test_vision_presence_trial_detects_presence_latency(monkeypatch) -> None:
    trial = importlib.import_module("noisebot_server.internal.ops.vision_presence_trial")

    states = iter(["candidate", "present", "present"])
    scores = iter([58, 72, 76])
    calls: list[str] = []

    def fake_get_json(base_url: str, path: str, timeout_s: float) -> dict:
        calls.append(path)
        if path == "api/vision/observe":
            return {
                "ok": True,
                "observation": {"valid": True},
                "presence": {"state": next(states), "score": next(scores)},
            }
        if path == "api/diag":
            return {"fps": 25.4}
        raise AssertionError(path)

    def fake_post_json(base_url: str, path: str, timeout_s: float) -> dict:
        calls.append(path)
        assert path == "api/camera/session/close"
        return {"ok": True}

    ticks = iter([0.0, 0.0, 0.0, 0.2, 0.2, 0.4, 0.4])
    monkeypatch.setattr(trial, "_get_json", fake_get_json)
    monkeypatch.setattr(trial, "_post_json", fake_post_json)

    result = trial.run_vision_presence_trial(
        firmware_url="http://192.168.1.30",
        mode="presence",
        duration_s=0.4,
        interval_s=0.2,
        max_latency_ms=500.0,
        fps_sample_delay_s=0.0,
        close_each_sample=False,
        min_fps_required=25.0,
        now_fn=lambda: next(ticks),
        sleep_fn=lambda _: None,
    )

    assert result.ok is True
    assert result.present_samples == 2
    assert result.candidate_samples == 1
    assert result.first_present_elapsed_ms == 200.0
    assert result.baseline_fps == 25.4
    assert result.fps_sample_delay_s == 0.0
    assert result.close_each_sample is False
    assert result.max_presence_score == 76
    assert calls.count("api/vision/observe") == 3


def test_vision_presence_trial_flags_absence_false_positive(monkeypatch) -> None:
    trial = importlib.import_module("noisebot_server.internal.ops.vision_presence_trial")

    def fake_get_json(base_url: str, path: str, timeout_s: float) -> dict:
        if path == "api/vision/observe":
            return {
                "ok": True,
                "observation": {"valid": True},
                "presence": {"state": "present", "score": 80},
            }
        if path == "api/diag":
            return {"fps": 25.0}
        raise AssertionError(path)

    monkeypatch.setattr(trial, "_get_json", fake_get_json)
    monkeypatch.setattr(
        trial,
        "_post_json",
        lambda base_url, path, timeout_s: {"ok": True},
    )

    ticks = iter([0.0, 0.02, 0.02])
    result = trial.run_vision_presence_trial(
        firmware_url="http://192.168.1.30",
        mode="absence",
        duration_s=0.0 + 0.01,
        interval_s=1.0,
        now_fn=lambda: next(ticks),
        sleep_fn=lambda _: None,
    )

    assert result.ok is False
    assert result.false_positive_count == 1
    assert result.errors == ["presence_false_positive:score=80"]
