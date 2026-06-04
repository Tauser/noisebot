"""Vision soak helper for repeated firmware observation checks."""

from __future__ import annotations

import json
import time
from dataclasses import dataclass
from typing import Any, Callable
from urllib.error import HTTPError, URLError
from urllib.parse import urljoin
from urllib.request import Request, urlopen


class VisionSoakError(RuntimeError):
    """Vision soak HTTP query failed."""


@dataclass(frozen=True)
class VisionSoakResult:
    ok: bool
    duration_s: float
    interval_s: float
    samples: int
    failures: int
    reboots: int
    valid_observations: int
    first_uptime_s: int | None
    last_uptime_s: int | None
    min_fps: float | None
    min_psram_free: int | None
    min_dma_free: int | None
    max_capture_ms: int | None
    max_jpeg_bytes: int | None
    presence_present_samples: int
    presence_candidate_samples: int
    presence_false_positive_count: int
    max_presence_score: int | None
    final_presence_state: str | None
    min_fps_required: float | None
    expect_absence: bool
    final_camera_ready: bool | None
    final_camera_active: bool | None
    final_close_ok: bool | None
    errors: list[str]

    def to_dict(self) -> dict[str, Any]:
        return {
            "ok": self.ok,
            "duration_s": round(self.duration_s, 3),
            "interval_s": round(self.interval_s, 3),
            "samples": self.samples,
            "failures": self.failures,
            "reboots": self.reboots,
            "valid_observations": self.valid_observations,
            "first_uptime_s": self.first_uptime_s,
            "last_uptime_s": self.last_uptime_s,
            "min_fps": self.min_fps,
            "min_psram_free": self.min_psram_free,
            "min_dma_free": self.min_dma_free,
            "max_capture_ms": self.max_capture_ms,
            "max_jpeg_bytes": self.max_jpeg_bytes,
            "presence_present_samples": self.presence_present_samples,
            "presence_candidate_samples": self.presence_candidate_samples,
            "presence_false_positive_count": self.presence_false_positive_count,
            "max_presence_score": self.max_presence_score,
            "final_presence_state": self.final_presence_state,
            "min_fps_required": self.min_fps_required,
            "expect_absence": self.expect_absence,
            "final_camera_ready": self.final_camera_ready,
            "final_camera_active": self.final_camera_active,
            "final_close_ok": self.final_close_ok,
            "errors": self.errors,
        }


def run_vision_soak(
    firmware_url: str,
    duration_s: float,
    interval_s: float,
    timeout_s: float = 8.0,
    *,
    expect_absence: bool = False,
    min_fps_required: float | None = None,
    now_fn: Callable[[], float] | None = None,
    sleep_fn: Callable[[float], None] | None = None,
) -> VisionSoakResult:
    """Run repeated `/api/vision/observe` checks against the firmware."""
    if duration_s <= 0.0:
        raise ValueError("duration_s must be positive")
    if interval_s <= 0.0:
        raise ValueError("interval_s must be positive")

    base_url = firmware_url.rstrip("/") + "/"
    now = now_fn or time.monotonic
    sleep = sleep_fn or time.sleep
    started = now()
    deadline = started + duration_s

    samples = 0
    failures = 0
    reboots = 0
    valid_observations = 0
    first_uptime: int | None = None
    last_uptime: int | None = None
    min_fps: float | None = None
    min_psram_free: int | None = None
    min_dma_free: int | None = None
    max_capture_ms: int | None = None
    max_jpeg_bytes: int | None = None
    presence_present_samples = 0
    presence_candidate_samples = 0
    presence_false_positive_count = 0
    max_presence_score: int | None = None
    final_presence_state: str | None = None
    errors: list[str] = []

    while True:
        loop_started = now()
        try:
            observation_payload = _get_json(base_url, "api/vision/observe", timeout_s)
            diag = _get_json(base_url, "api/diag", timeout_s)
            camera = _get_json(base_url, "api/camera/status", timeout_s)

            observation = observation_payload.get("observation", {})
            if not observation_payload.get("ok", False):
                failures += 1
                errors.append(str(observation_payload.get("error", "vision_not_ok")))
            elif not isinstance(observation, dict) or not observation.get("valid", False):
                failures += 1
                errors.append("observation_invalid")
            else:
                valid_observations += 1

            presence = observation_payload.get("presence", {})
            if isinstance(presence, dict):
                state = str(presence.get("state", "unknown"))
                score = _int(presence.get("score"))
                final_presence_state = state
                max_presence_score = (
                    score if max_presence_score is None else max(max_presence_score, score)
                )
                if state == "present":
                    presence_present_samples += 1
                    if expect_absence:
                        presence_false_positive_count += 1
                        errors.append(f"presence_false_positive:score={score}")
                elif state == "candidate":
                    presence_candidate_samples += 1

            uptime = _int(diag.get("uptime_s"))
            if first_uptime is None:
                first_uptime = uptime
            if last_uptime is not None and uptime < last_uptime:
                reboots += 1
                errors.append(f"uptime_reset:{last_uptime}->{uptime}")
            last_uptime = uptime

            fps = _float(diag.get("fps"))
            if fps > 0.0:
                min_fps = fps if min_fps is None else min(min_fps, fps)
                if min_fps_required is not None and fps < min_fps_required:
                    errors.append(f"fps_below_min:{fps:.1f}<{min_fps_required:.1f}")

            memory = diag.get("memory", {})
            if isinstance(memory, dict):
                psram_free = _int(memory.get("psram_free"))
                min_psram_free = (
                    psram_free if min_psram_free is None else min(min_psram_free, psram_free)
                )

            dma_free = _int(camera.get("heap_dma_free"))
            min_dma_free = dma_free if min_dma_free is None else min(min_dma_free, dma_free)

            capture_ms = _int(observation.get("capture_ms"))
            jpeg_bytes = _int(observation.get("jpeg_bytes"))
            max_capture_ms = (
                capture_ms if max_capture_ms is None else max(max_capture_ms, capture_ms)
            )
            max_jpeg_bytes = (
                jpeg_bytes if max_jpeg_bytes is None else max(max_jpeg_bytes, jpeg_bytes)
            )
        except Exception as exc:
            failures += 1
            errors.append(str(exc))

        samples += 1
        if loop_started >= deadline:
            break
        sleep(max(0.0, min(interval_s, deadline - now())))

    close_ok: bool | None = None
    final_ready: bool | None = None
    final_active: bool | None = None
    try:
        close_payload = _post_json(base_url, "api/camera/session/close", timeout_s)
        close_ok = bool(close_payload.get("ok", False))
        camera = _get_json(base_url, "api/camera/status", timeout_s)
        final_ready = bool(camera.get("ready", False))
        final_active = bool(camera.get("active", False))
    except Exception as exc:
        failures += 1
        errors.append(f"close:{exc}")

    ok = (
        samples > 0
        and failures == 0
        and reboots == 0
        and valid_observations == samples
        and presence_false_positive_count == 0
        and (min_fps_required is None or (min_fps is not None and min_fps >= min_fps_required))
        and close_ok is True
        and final_ready is False
        and final_active is False
    )
    return VisionSoakResult(
        ok=ok,
        duration_s=now() - started,
        interval_s=interval_s,
        samples=samples,
        failures=failures,
        reboots=reboots,
        valid_observations=valid_observations,
        first_uptime_s=first_uptime,
        last_uptime_s=last_uptime,
        min_fps=min_fps,
        min_psram_free=min_psram_free,
        min_dma_free=min_dma_free,
        max_capture_ms=max_capture_ms,
        max_jpeg_bytes=max_jpeg_bytes,
        presence_present_samples=presence_present_samples,
        presence_candidate_samples=presence_candidate_samples,
        presence_false_positive_count=presence_false_positive_count,
        max_presence_score=max_presence_score,
        final_presence_state=final_presence_state,
        min_fps_required=min_fps_required,
        expect_absence=expect_absence,
        final_camera_ready=final_ready,
        final_camera_active=final_active,
        final_close_ok=close_ok,
        errors=errors[:20],
    )


def format_vision_soak_markdown(result: VisionSoakResult) -> str:
    status = "OK" if result.ok else "FALHOU"
    return "\n".join([
        "# Vision Soak",
        f"- Status: {status}",
        f"- Amostras: {result.valid_observations}/{result.samples}",
        f"- Falhas: {result.failures}",
        f"- Reboots: {result.reboots}",
        f"- Uptime: {result.first_uptime_s} -> {result.last_uptime_s}",
        f"- FPS mínimo: {result.min_fps}",
        f"- PSRAM mínima: {result.min_psram_free}",
        f"- DMA mínima: {result.min_dma_free}",
        f"- Captura máxima: {result.max_capture_ms} ms",
        f"- JPEG máximo: {result.max_jpeg_bytes} bytes",
        f"- Presença final: {result.final_presence_state}",
        f"- Presença present/candidate: {result.presence_present_samples}/{result.presence_candidate_samples}",
        f"- Falsos positivos de presença: {result.presence_false_positive_count}",
        f"- Score máximo de presença: {result.max_presence_score}",
        f"- FPS mínimo exigido: {result.min_fps_required}",
        f"- Ausência esperada: {result.expect_absence}",
        f"- Câmera final: ready={result.final_camera_ready}, active={result.final_camera_active}",
        f"- Close final: {result.final_close_ok}",
        f"- Erros: {result.errors}",
    ])


def format_vision_soak_json(result: VisionSoakResult) -> str:
    return json.dumps(result.to_dict(), ensure_ascii=False, indent=2)


def _get_json(base_url: str, path: str, timeout_s: float) -> dict[str, Any]:
    return _request_json(base_url, path, timeout_s, method="GET")


def _post_json(base_url: str, path: str, timeout_s: float) -> dict[str, Any]:
    return _request_json(base_url, path, timeout_s, method="POST")


def _request_json(base_url: str, path: str, timeout_s: float, method: str) -> dict[str, Any]:
    url = urljoin(base_url, path.lstrip("/"))
    request = Request(url, method=method, headers={"User-Agent": "NoiseBot-Server/0.1"})
    try:
        with urlopen(request, timeout=timeout_s) as response:
            data = response.read().decode("utf-8")
    except (HTTPError, URLError, TimeoutError, OSError) as exc:
        raise VisionSoakError(f"{path}: {exc}") from exc
    try:
        payload = json.loads(data)
    except json.JSONDecodeError as exc:
        raise VisionSoakError(f"{path}: resposta nao e JSON") from exc
    if not isinstance(payload, dict):
        raise VisionSoakError(f"{path}: resposta invalida")
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


__all__ = [
    "VisionSoakError",
    "VisionSoakResult",
    "format_vision_soak_json",
    "format_vision_soak_markdown",
    "run_vision_soak",
]
