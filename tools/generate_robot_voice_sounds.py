"""Generate NoiseBot character sounds as WAV assets.

Output format: PCM 16-bit mono 16 kHz, matching the firmware audio path.
The goal is a small coherent robot "voice", not plain sine beeps.
"""

from __future__ import annotations

import math
import random
import wave
from pathlib import Path


SAMPLE_RATE = 16_000
OUT_DIR = Path(__file__).resolve().parents[1] / "assets" / "sounds" / "robot_voice"


def clamp(v: float, lo: float = -1.0, hi: float = 1.0) -> float:
    return max(lo, min(hi, v))


def env(n: int, total: int, attack: float = 0.035, release: float = 0.16) -> float:
    if total <= 1:
        return 0.0
    t = n / SAMPLE_RATE
    d = total / SAMPLE_RATE
    a = min(attack, d * 0.35)
    r = min(release, d * 0.45)
    if t < a:
        x = t / max(a, 1e-6)
        return x * x * (3.0 - 2.0 * x)
    if t > d - r:
        x = (d - t) / max(r, 1e-6)
        return max(0.0, x * x * (3.0 - 2.0 * x))
    return 1.0


def normalize(samples: list[float], peak: float = 0.82) -> list[float]:
    m = max((abs(x) for x in samples), default=1.0)
    if m < 1e-6:
        return samples
    gain = peak / m
    return [clamp(x * gain) for x in samples]


def write_wav(name: str, samples: list[float]) -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    path = OUT_DIR / name
    samples = normalize(samples)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        frames = bytearray()
        for s in samples:
            frames += int(clamp(s) * 32767.0).to_bytes(2, "little", signed=True)
        w.writeframes(frames)
    print(path)


def silence(ms: int) -> list[float]:
    return [0.0] * int(SAMPLE_RATE * ms / 1000)


def glide(
    seconds: float,
    start_hz: float,
    end_hz: float,
    *,
    amp: float = 0.5,
    harmonics: tuple[float, ...] = (1.0, 0.32, 0.12),
    vibrato_hz: float = 5.0,
    vibrato_depth: float = 0.008,
    noise: float = 0.015,
    attack: float = 0.03,
    release: float = 0.14,
) -> list[float]:
    total = int(seconds * SAMPLE_RATE)
    out: list[float] = []
    phase = 0.0
    rng = random.Random(173)
    lp_noise = 0.0
    for n in range(total):
        p = n / max(1, total - 1)
        curve = p * p * (3.0 - 2.0 * p)
        hz = start_hz + (end_hz - start_hz) * curve
        hz *= 1.0 + math.sin(2.0 * math.pi * vibrato_hz * n / SAMPLE_RATE) * vibrato_depth
        phase += 2.0 * math.pi * hz / SAMPLE_RATE
        voice = 0.0
        for i, h_amp in enumerate(harmonics, start=1):
            voice += h_amp * math.sin(phase * i + 0.13 * i)
        breath = rng.uniform(-1.0, 1.0)
        lp_noise = (lp_noise * 0.92) + (breath * 0.08)
        shaped = math.tanh(voice * 0.82) + lp_noise * noise
        out.append(shaped * amp * env(n, total, attack, release))
    return out


def syllables(parts: list[tuple[float, float, float, float]]) -> list[float]:
    out: list[float] = []
    for seconds, start, end, amp in parts:
        out += glide(seconds, start, end, amp=amp)
        out += silence(28)
    return out


def shimmer(seconds: float, base_hz: float, amp: float = 0.25) -> list[float]:
    total = int(seconds * SAMPLE_RATE)
    out = [0.0] * total
    for offset, mult in ((0.0, 1.0), (0.07, 1.5), (0.14, 2.0)):
        start = int(offset * SAMPLE_RATE)
        layer = glide(max(0.01, seconds - offset), base_hz * mult, base_hz * mult * 1.25,
                      amp=amp / mult, harmonics=(1.0, 0.2), noise=0.01)
        for i, v in enumerate(layer):
            if start + i < total:
                out[start + i] += v
    return out


def purr(seconds: float, hz: float = 95.0, amp: float = 0.42) -> list[float]:
    total = int(seconds * SAMPLE_RATE)
    rng = random.Random(884)
    out: list[float] = []
    phase = 0.0
    lp = 0.0
    for n in range(total):
        lfo = 0.58 + 0.42 * math.sin(2.0 * math.pi * 7.5 * n / SAMPLE_RATE)
        phase += 2.0 * math.pi * (hz + 8.0 * lfo) / SAMPLE_RATE
        buzz = math.sin(phase) + 0.35 * math.sin(phase * 2.0) + 0.12 * math.sin(phase * 3.0)
        lp = lp * 0.88 + rng.uniform(-1.0, 1.0) * 0.12
        out.append((math.tanh(buzz * 1.3) * lfo + lp * 0.13) * amp * env(n, total, 0.08, 0.22))
    return out


def pulse_train(seconds: float, hz: float, count: int, amp: float = 0.42) -> list[float]:
    total = int(seconds * SAMPLE_RATE)
    out = [0.0] * total
    spacing = total // max(1, count)
    for k in range(count):
        start = k * spacing
        layer = glide(0.18, hz * (1.0 + k * 0.035), hz * (0.92 + k * 0.03),
                      amp=amp, harmonics=(1.0, 0.25, 0.08), noise=0.02)
        for i, v in enumerate(layer):
            if start + i < total:
                out[start + i] += v
    return out


def main() -> None:
    assets = {
        "greet_01.wav": syllables([(0.22, 420, 690, 0.54), (0.24, 610, 880, 0.48)]),
        "greet_02.wav": syllables([(0.34, 360, 560, 0.42), (0.28, 520, 470, 0.34)]),
        "greet_03.wav": shimmer(0.72, 430, 0.35),
        "touch_respond_01.wav": purr(0.62, 92, 0.36),
        "touch_respond_02.wav": purr(0.95, 86, 0.34) + glide(0.22, 310, 390, amp=0.2),
        "touch_respond_03.wav": syllables([(0.18, 260, 330, 0.32), (0.36, 300, 520, 0.38)]),
        "idle_01.wav": glide(0.42, 210, 260, amp=0.18, harmonics=(1.0, 0.18), noise=0.035),
        "curious.wav": syllables([(0.18, 360, 620, 0.38), (0.28, 520, 900, 0.46)]),
        "happy.wav": shimmer(0.82, 520, 0.38),
        "focused.wav": pulse_train(0.78, 180, 3, 0.27),
        "thinking.wav": pulse_train(1.15, 260, 5, 0.22),
        "listen_start.wav": glide(0.46, 310, 740, amp=0.38, noise=0.025),
        "listen_end.wav": glide(0.34, 620, 360, amp=0.32, noise=0.02),
        "wake_up.wav": shimmer(0.68, 360, 0.42),
        "sleep_enter.wav": glide(0.92, 420, 115, amp=0.36, harmonics=(1.0, 0.2), noise=0.03),
        "error_01.wav": syllables([(0.22, 420, 260, 0.38), (0.26, 250, 310, 0.32)]),
        "timer_done.wav": shimmer(0.72, 470, 0.36),
        "reminder_due.wav": syllables([(0.24, 390, 650, 0.38), (0.34, 610, 720, 0.36)]),
        "alarm_due.wav": pulse_train(1.05, 520, 4, 0.34),
    }

    for name, samples in assets.items():
        write_wav(name, samples)


if __name__ == "__main__":
    main()
