"""Presence validation helper for firmware vision shadow mode."""

from __future__ import annotations

import json
import time
from dataclasses import dataclass
from typing import Any, Callable, Literal
from urllib.error import HTTPError, URLError
from urllib.parse import urljoin
from urllib.request import Request, urlopen

VisionPresenceMode = Literal["absence", "presence", "lost"]


class VisionPresenceTrialError(RuntimeError):
    """Vision presence trial HTTP query failed."""


@dataclass(frozen=True)
class VisionPresenceTrialResult:
    ok: bool
    mode: VisionPresenceMode
    duration_s: float
    interval_s: float
    samples: int
    valid_observations: int
    failures: int
    present_samples: int
    candidate_samples: int
    absent_samples: int
    false_positive_count: int
    first_present_elapsed_ms: float | None
    first_absent_elapsed_ms: float | None
    max_presence_score: int | None
    final_presence_state: str | None
    baseline_fps: float | None
    min_fps: float | None
    fps_sample_delay_s: float
    close_each_sample: bool
    min_fps_required: float | None
    max_latency_ms: float | None
    final_close_ok: bool | None
    errors: list[str]

    def to_dict(self) -> dict[str, Any]:
        return {
            "ok": self.ok,
            "mode": self.mode,
            "duration_s": round(self.duration_s, 3),
            "interval_s": round(self.interval_s, 3),
            "samples": self.samples,
            "valid_observations": self.valid_observations,
            "failures": self.failures,
            "present_samples": self.present_samples,
            "candidate_samples": self.candidate_samples,
            "absent_samples": self.absent_samples,
            "false_positive_count": self.false_positive_count,
            "first_present_elapsed_ms": _round_optional(self.first_present_elapsed_ms),
            "first_absent_elapsed_ms": _round_optional(self.first_absent_elapsed_ms),
            "max_presence_score": self.max_presence_score,
            "final_presence_state": self.final_presence_state,
            "baseline_fps": self.baseline_fps,
            "min_fps": self.min_fps,
            "fps_sample_delay_s": round(self.fps_sample_delay_s, 3),
            "close_each_sample": self.close_each_sample,
            "min_fps_required": self.min_fps_required,
            "max_latency_ms": self.max_latency_ms,
            "final_close_ok": self.final_close_ok,
            "errors": self.errors,
        }


def run_vision_presence_trial(
    firmware_url: str,
    mode: VisionPresenceMode,
    duration_s: float,
    interval_s: float,
    timeout_s: float = 8.0,
    *,
    max_latency_ms: float | None = None,
    fps_sample_delay_s: float = 0.0,
    close_each_sample: bool = False,
    min_fps_required: float | None = None,
    now_fn: Callable[[], float] | None = None,
    sleep_fn: Callable[[float], None] | None = None,
) -> VisionPresenceTrialResult:
    """Run a focused presence validation trial against `/api/vision/observe`."""
    if mode not in ("absence", "presence", "lost"):
        raise ValueError("mode must be absence, presence or lost")
    if duration_s <= 0.0:
        raise ValueError("duration_s must be positive")
    if interval_s <= 0.0:
        raise ValueError("interval_s must be positive")
    if fps_sample_delay_s < 0.0:
        raise ValueError("fps_sample_delay_s must not be negative")

    base_url = firmware_url.rstrip("/") + "/"
    now = now_fn or time.monotonic
    sleep = sleep_fn or time.sleep
    started = now()
    deadline = started + duration_s

    samples = 0
    valid_observations = 0
    failures = 0
    present_samples = 0
    candidate_samples = 0
    absent_samples = 0
    false_positive_count = 0
    first_present_elapsed_ms: float | None = None
    first_absent_elapsed_ms: float | None = None
    max_presence_score: int | None = None
    final_presence_state: str | None = None
    baseline_fps: float | None = None
    min_fps: float | None = None
    errors: list[str] = []

    try:
        baseline_value = _read_fps(base_url, timeout_s)
        if baseline_value > 0.0:
            baseline_fps = baseline_value
    except Exception as exc:
        failures += 1
        errors.append(f"baseline:{exc}")

    while True:
        loop_started = now()
        elapsed_ms = (loop_started - started) * 1000.0
        try:
            payload = _get_json(base_url, "api/vision/observe", timeout_s)
            if close_each_sample:
                close_payload = _post_json(base_url, "api/camera/session/close", timeout_s)
                if not close_payload.get("ok", False):
                    failures += 1
                    errors.append("close_each_sample_not_ok")
            if fps_sample_delay_s > 0.0:
                sleep(fps_sample_delay_s)
            observation = payload.get("observation", {})
            presence = payload.get("presence", {})

            if not payload.get("ok", False):
                failures += 1
                errors.append(str(payload.get("error", "vision_not_ok")))
            elif not isinstance(observation, dict) or not observation.get("valid", False):
                failures += 1
                errors.append("observation_invalid")
            else:
                valid_observations += 1

            if isinstance(presence, dict):
                state = str(presence.get("state", "unknown"))
                score = _int(presence.get("score"))
                final_presence_state = state
                max_presence_score = (
                    score if max_presence_score is None else max(max_presence_score, score)
                )
                if state == "present":
                    present_samples += 1
                    if first_present_elapsed_ms is None:
                        first_present_elapsed_ms = elapsed_ms
                    if mode == "absence":
                        false_positive_count += 1
                        errors.append(f"presence_false_positive:score={score}")
                elif state == "candidate":
                    candidate_samples += 1
                elif state == "absent":
                    absent_samples += 1
                    if first_absent_elapsed_ms is None:
                        first_absent_elapsed_ms = elapsed_ms

            fps = _read_fps(base_url, timeout_s)
            if fps > 0.0:
                min_fps = fps if min_fps is None else min(min_fps, fps)
                if min_fps_required is not None and fps < min_fps_required:
                    errors.append(f"fps_below_min:{fps:.1f}<{min_fps_required:.1f}")
        except Exception as exc:
            failures += 1
            errors.append(str(exc))

        samples += 1
        if loop_started >= deadline:
            break
        sleep(max(0.0, min(interval_s, deadline - now())))

    close_ok: bool | None = None
    try:
        close_payload = _post_json(base_url, "api/camera/session/close", timeout_s)
        close_ok = bool(close_payload.get("ok", False))
    except Exception as exc:
        failures += 1
        errors.append(f"close:{exc}")

    latency_ok = True
    if mode == "presence":
        latency_ok = first_present_elapsed_ms is not None
        if max_latency_ms is not None:
            latency_ok = latency_ok and first_present_elapsed_ms <= max_latency_ms
    elif mode == "lost":
        latency_ok = first_absent_elapsed_ms is not None
        if max_latency_ms is not None:
            latency_ok = latency_ok and first_absent_elapsed_ms <= max_latency_ms

    fps_ok = min_fps_required is None or (min_fps is not None and min_fps >= min_fps_required)
    ok = (
        samples > 0
        and failures == 0
        and valid_observations == samples
        and false_positive_count == 0
        and latency_ok
        and fps_ok
        and close_ok is True
    )

    return VisionPresenceTrialResult(
        ok=ok,
        mode=mode,
        duration_s=now() - started,
        interval_s=interval_s,
        samples=samples,
        valid_observations=valid_observations,
        failures=failures,
        present_samples=present_samples,
        candidate_samples=candidate_samples,
        absent_samples=absent_samples,
        false_positive_count=false_positive_count,
        first_present_elapsed_ms=first_present_elapsed_ms,
        first_absent_elapsed_ms=first_absent_elapsed_ms,
        max_presence_score=max_presence_score,
        final_presence_state=final_presence_state,
        baseline_fps=baseline_fps,
        min_fps=min_fps,
        fps_sample_delay_s=fps_sample_delay_s,
        close_each_sample=close_each_sample,
        min_fps_required=min_fps_required,
        max_latency_ms=max_latency_ms,
        final_close_ok=close_ok,
        errors=errors[:20],
    )


def format_vision_presence_trial_markdown(result: VisionPresenceTrialResult) -> str:
    status = "OK" if result.ok else "FALHOU"
    return "\n".join([
        "# Vision Presence Trial",
        f"- Status: {status}",
        f"- Modo: {result.mode}",
        f"- Amostras: {result.valid_observations}/{result.samples}",
        f"- Falhas: {result.failures}",
        f"- Presença present/candidate/absent: {result.present_samples}/{result.candidate_samples}/{result.absent_samples}",
        f"- Falsos positivos: {result.false_positive_count}",
        f"- Primeiro present: {result.first_present_elapsed_ms} ms",
        f"- Primeiro absent: {result.first_absent_elapsed_ms} ms",
        f"- Score máximo: {result.max_presence_score}",
        f"- Estado final: {result.final_presence_state}",
        f"- FPS baseline: {result.baseline_fps}",
        f"- FPS mínimo: {result.min_fps}",
        f"- Delay de amostragem FPS: {result.fps_sample_delay_s} s",
        f"- Fecha câmera a cada amostra: {result.close_each_sample}",
        f"- FPS mínimo exigido: {result.min_fps_required}",
        f"- Latência máxima exigida: {result.max_latency_ms} ms",
        f"- Close final: {result.final_close_ok}",
        f"- Erros: {result.errors}",
    ])


def format_vision_presence_trial_json(result: VisionPresenceTrialResult) -> str:
    return json.dumps(result.to_dict(), ensure_ascii=False, indent=2)


def _get_json(base_url: str, path: str, timeout_s: float) -> dict[str, Any]:
    return _request_json(base_url, path, timeout_s, method="GET")


def _post_json(base_url: str, path: str, timeout_s: float) -> dict[str, Any]:
    return _request_json(base_url, path, timeout_s, method="POST")


def _read_fps(base_url: str, timeout_s: float) -> float:
    try:
        render = _get_json(base_url, "api/render/status", timeout_s)
        fps = _float(render.get("fps"))
        if fps > 0.0:
            return fps
    except VisionPresenceTrialError:
        pass
    diag = _get_json(base_url, "api/diag", timeout_s)
    return _float(diag.get("fps"))


def _request_json(base_url: str, path: str, timeout_s: float, method: str) -> dict[str, Any]:
    url = urljoin(base_url, path.lstrip("/"))
    request = Request(url, method=method, headers={"User-Agent": "NoiseBot-Server/0.1"})
    try:
        with urlopen(request, timeout=timeout_s) as response:
            data = response.read().decode("utf-8")
    except (HTTPError, URLError, TimeoutError, OSError) as exc:
        raise VisionPresenceTrialError(f"{path}: {exc}") from exc
    try:
        payload = json.loads(data)
    except json.JSONDecodeError as exc:
        raise VisionPresenceTrialError(f"{path}: resposta nao e JSON") from exc
    if not isinstance(payload, dict):
        raise VisionPresenceTrialError(f"{path}: resposta invalida")
    return payload


def _int(value: Any) -> int:
    try:
        return int(value or 0)
    except (TypeError, ValueError):
        return 0


def _float(value: Any) -> float:
    try:
        return float(value or 0.0)
    except (TypeError, ValueError):
        return 0.0


def _round_optional(value: float | None) -> float | None:
    return None if value is None else round(value, 3)


__all__ = [
    "VisionPresenceTrialError",
    "VisionPresenceTrialResult",
    "format_vision_presence_trial_json",
    "format_vision_presence_trial_markdown",
    "run_vision_presence_trial",
]
