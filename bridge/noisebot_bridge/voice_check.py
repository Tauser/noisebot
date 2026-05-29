from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
import subprocess
import sys

import numpy as np

from .replay import run_replay
from .stt import SttResult


ROOT = Path(__file__).resolve().parents[2]
BRIDGE_DIR = ROOT / "bridge"
VOICE_SAMPLES = ROOT / "voice_samples"
VOICE_REPLAY_BASELINE = ROOT / "docs" / "VOICE_REPLAY_BASELINE.json"


class GoodFixtureStt:
    ready = True

    def empty_result(self):
        return SttResult(backend="voice-check")

    def transcribe(self, pcm):
        return SttResult(
            text="que horas sao",
            no_speech_prob=0.01,
            avg_logprob=-0.2,
            compression_ratio=1.0,
            backend="voice-check",
        )


class RejectedFixtureStt(GoodFixtureStt):
    def transcribe(self, pcm):
        return SttResult(
            text="",
            no_speech_prob=0.99,
            avg_logprob=-2.0,
            compression_ratio=0.0,
            backend="voice-check",
        )


class NoneLlm:
    ready = False


class FakeTts:
    def synthesize(self, text):
        return np.zeros(256, dtype=np.int16)


@dataclass(frozen=True)
class VoiceCheckResult:
    name: str
    ok: bool
    detail: str


def run_pytest_suite() -> VoiceCheckResult:
    env = dict(os.environ)
    env["PYTHONPATH"] = str(BRIDGE_DIR)
    completed = subprocess.run(
        [sys.executable, "-m", "pytest", "bridge/tests"],
        cwd=str(ROOT),
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )
    output = completed.stdout + completed.stderr
    last_line = next((line for line in reversed(output.splitlines()) if " passed" in line), "")
    detail = last_line or f"pytest exit={completed.returncode}"
    return VoiceCheckResult("pytest", completed.returncode == 0, detail)


def run_voice_replay_baseline() -> VoiceCheckResult:
    baseline = json.loads(VOICE_REPLAY_BASELINE.read_text(encoding="utf-8"))
    failures: list[str] = []
    checked = 0

    for sample in baseline["good_samples"]:
        path = VOICE_SAMPLES / sample["file"]
        result = run_replay(str(path), GoodFixtureStt(), NoneLlm(), FakeTts(), dry_run=True)
        checked += 1
        if result.session.outcome != sample["expected_outcome"]:
            failures.append(f"{sample['file']}: {result.session.outcome} != {sample['expected_outcome']}")

    for sample in baseline["rejected_samples"]:
        path = VOICE_SAMPLES / sample["file"]
        result = run_replay(str(path), RejectedFixtureStt(), NoneLlm(), FakeTts(), dry_run=False)
        checked += 1
        if result.session.outcome != sample["expected_outcome"]:
            failures.append(f"{sample['file']}: {result.session.outcome} != {sample['expected_outcome']}")

    if failures:
        return VoiceCheckResult("voice_replay_baseline", False, "; ".join(failures))
    return VoiceCheckResult("voice_replay_baseline", True, f"{checked} fixtures ok")


def run_voice_check() -> list[VoiceCheckResult]:
    return [run_pytest_suite(), run_voice_replay_baseline()]


def main() -> int:
    results = run_voice_check()
    for result in results:
        status = "OK" if result.ok else "FAIL"
        print(f"{status} {result.name}: {result.detail}")
    return 0 if all(r.ok for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
