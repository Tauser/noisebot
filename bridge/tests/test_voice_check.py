from noisebot_bridge.voice_check import run_voice_replay_baseline


def test_voice_replay_baseline_check_passes():
    result = run_voice_replay_baseline()

    assert result.ok
    assert result.name == "voice_replay_baseline"
    assert result.detail == "4 fixtures ok"
