from __future__ import annotations

import asyncio
import importlib
import sys
import struct
from pathlib import Path


def _ensure_bridgev2_path() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    bridge_v2_path = repo_root / "bridge_v2"
    bridge_v2_str = str(bridge_v2_path)
    if bridge_v2_path.exists() and bridge_v2_str not in sys.path:
        sys.path.insert(0, bridge_v2_str)


def _make_server_config(
    *,
    host: str | None = None,
    port: int = 9000,
    uart: str | None = None,
    dry_run: bool = True,
    piper_model: str = "",
):
    config_module = importlib.import_module("noisebot_server.config")

    return config_module.NoiseBotServerConfig(
        transport=config_module.TransportConfig(
            host=host,
            port=port,
            uart=uart,
            baudrate=1000000,
        ),
        llm=config_module.LlmConfig(
            provider=config_module.LlmProvider.NONE,
            model="none",
            timeout_s=10.0,
            max_output_tokens=256,
            max_reply_chars=180,
            ollama_base_url="http://127.0.0.1:11434",
            ollama_think=False,
            openai_key_configured=False,
            gemini_key_configured=False,
        ),
        pipeline_mode=config_module.PipelineMode.LOCAL_ONLY,
        stt=config_module.SttConfig(
            model="small",
            backend="faster",
            device="cpu",
            compute_type="int8",
        ),
        tts=config_module.TtsConfig(
            piper_executable="piper",
            piper_model=piper_model,
            cache_size=64,
            sample_rate=16000,
            target_peak=12000,
        ),
        audio=config_module.AudioConfig(
            chunk_samples=256,
            sample_rate=16000,
            min_transcribe_rms=140.0,
            min_transcribe_peak=1600,
            min_utterance_samples=8000,
            max_no_speech_prob=0.75,
            min_avg_logprob=-1.10,
            max_compression_ratio=2.60,
        ),
        reconnect=config_module.ReconnectConfig(
            delay_s=0.05,
            max_delay_s=0.2,
            connect_timeout_s=2.0,
        ),
        ops=config_module.OpsConfig(
            port=8765,
            token_configured=False,
        ),
        log_level=config_module.LogLevel.INFO,
        dry_run=dry_run,
        replay_path=None,
    )


def test_bridgev2_reference_path_allows_application_import() -> None:
    _ensure_bridgev2_path()

    app_module = importlib.import_module("noisebot_server.app")

    assert hasattr(app_module, "NoiseBotServer")


def test_server_entrypoint_exposes_server_cli() -> None:
    _ensure_bridgev2_path()

    cli_module = importlib.import_module("noisebot_server.__main__")

    assert callable(cli_module.main)


def test_server_cli_parses_runtime_flags() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "--host", "192.168.1.30",
        "--port", "9000",
        "--pipeline", "local_only",
        "--llm", "ollama",
        "--model", "qwen2.5:7b",
        "--log-file", "stderr",
    ])

    assert args.command is None
    assert args.host == "192.168.1.30"
    assert args.port == 9000
    assert args.pipeline == "local_only"
    assert args.llm == "ollama"
    assert args.model == "qwen2.5:7b"
    assert args.log_file == "stderr"


def test_server_cli_applies_env_overrides(monkeypatch) -> None:
    cli = importlib.import_module("noisebot_server.cli")

    for key in (
        "NOISEBOT_HOST",
        "NOISEBOT_PORT",
        "NOISEBOT_DRY_RUN",
        "NOISEBOT_PIPELINE_MODE",
        "NOISEBOT_LLM_PROVIDER",
        "NOISEBOT_LLM_MODEL",
    ):
        monkeypatch.delenv(key, raising=False)

    args = cli.parse_args([
        "--host", "10.0.0.2",
        "--port", "9010",
        "--dry-run",
        "--pipeline", "local_only",
        "--llm", "none",
        "--model", "none",
    ])

    cli.apply_env_overrides(args)

    import os

    assert os.environ["NOISEBOT_HOST"] == "10.0.0.2"
    assert os.environ["NOISEBOT_PORT"] == "9010"
    assert os.environ["NOISEBOT_DRY_RUN"] == "true"
    assert os.environ["NOISEBOT_PIPELINE_MODE"] == "local_only"
    assert os.environ["NOISEBOT_LLM_PROVIDER"] == "none"
    assert os.environ["NOISEBOT_LLM_MODEL"] == "none"


def test_server_config_is_server_owned() -> None:
    _ensure_bridgev2_path()

    server_config = importlib.import_module("noisebot_server.config")
    bridge_config = importlib.import_module("bridgev2.config")

    assert hasattr(server_config, "NoiseBotServerConfig")
    assert not hasattr(server_config, "BridgeV2Config")
    assert server_config.LlmProvider.OLLAMA.value == bridge_config.LlmProvider.OLLAMA.value
    assert server_config.PipelineMode.LOCAL_ONLY.value == (
        bridge_config.PipelineMode.LOCAL_ONLY.value
    )


def test_server_cli_runs_debug_transcript_without_bridge_entrypoint(monkeypatch) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    manual = importlib.import_module("noisebot_server.internal.debug.manual")

    calls: dict[str, object] = {}

    async def fake_run_transcript_debug(text: str, turn_id: int = 1) -> int:
        calls["text"] = text
        calls["turn_id"] = turn_id
        return 7

    monkeypatch.setattr(manual, "run_transcript_debug", fake_run_transcript_debug)

    try:
        cli.main(["debug", "transcript", "oi noise", "--turn-id", "42"])
    except SystemExit as exc:
        assert exc.code == 7
    else:
        raise AssertionError("debug command must exit with helper return code")

    assert calls == {"text": "oi noise", "turn_id": 42}


def test_server_cli_runs_service_status_without_bridge_entrypoint(
    monkeypatch,
    capsys,
) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    manager_module = importlib.import_module("noisebot_server.internal.service.manager")

    class FakeManager:
        def install(self) -> None:
            raise AssertionError("unexpected install")

        def uninstall(self) -> None:
            raise AssertionError("unexpected uninstall")

        def status(self) -> str:
            return "ok server"

        def start(self) -> None:
            raise AssertionError("unexpected start")

        def stop(self) -> None:
            raise AssertionError("unexpected stop")

    monkeypatch.setattr(manager_module, "get_manager", lambda: FakeManager())

    cli.main(["service", "status"])

    captured = capsys.readouterr()
    assert "ok server" in captured.out


def test_server_debug_msg_name_uses_server_boundary() -> None:
    manual = importlib.import_module("noisebot_server.internal.debug.manual")
    protocol = importlib.import_module("noisebot_server.internal.transport.protocol")

    assert manual.msg_name(protocol.MSG_HELLO) == "HELLO"
    assert manual.msg_name(0xFE) == "0xFE"


def test_server_service_manager_uses_server_identity() -> None:
    manager = importlib.import_module("noisebot_server.internal.service.manager")

    assert manager.TASK_NAME == "NoiseBot Server"
    assert manager.SERVICE_NAME == "noisebot-server"
    assert "-m noisebot_server" in manager.SYSTEMD_TEMPLATE
    assert "SyslogIdentifier=noisebot-server" in manager.SYSTEMD_TEMPLATE


def test_server_service_selects_windows_manager(monkeypatch) -> None:
    manager = importlib.import_module("noisebot_server.internal.service.manager")

    monkeypatch.setattr(manager.platform, "system", lambda: "Windows")

    assert isinstance(manager.get_manager(), manager.WindowsTaskSchedulerManager)


def test_server_service_selects_systemd_manager(monkeypatch) -> None:
    manager = importlib.import_module("noisebot_server.internal.service.manager")

    monkeypatch.setattr(manager.platform, "system", lambda: "Linux")

    assert isinstance(manager.get_manager(), manager.SystemdManager)


def test_server_service_windows_install_uses_noisebot_module(monkeypatch) -> None:
    manager = importlib.import_module("noisebot_server.internal.service.manager")
    calls: list[list[str]] = []

    class Result:
        returncode = 0
        stdout = ""
        stderr = ""

    def fake_run(cmd: list[str], **_: object) -> Result:
        calls.append(cmd)
        return Result()

    monkeypatch.setattr(manager.subprocess, "run", fake_run)
    workdir = manager.Path("D:/NoiseBot")
    monkeypatch.setattr(manager, "service_workdir", lambda: workdir)

    manager.WindowsTaskSchedulerManager().install()

    script = calls[0][-1]
    assert manager.TASK_NAME in script
    assert "-m noisebot_server" in script
    assert str(workdir) in script


def test_server_service_systemd_install_writes_noisebot_unit(
    monkeypatch,
    tmp_path,
) -> None:
    manager = importlib.import_module("noisebot_server.internal.service.manager")
    calls: list[list[str]] = []

    class Result:
        returncode = 0
        stdout = ""
        stderr = ""

    def fake_run(cmd: list[str], **_: object) -> Result:
        calls.append(cmd)
        return Result()

    service = manager.SystemdManager()
    monkeypatch.setattr(type(service), "_unit_dir", property(lambda _: tmp_path))
    monkeypatch.setattr(manager.subprocess, "run", fake_run)
    workdir = manager.Path("/noisebot")
    monkeypatch.setattr(manager, "service_workdir", lambda: workdir)

    service.install()

    unit_file = tmp_path / f"{manager.SERVICE_NAME}.service"
    content = unit_file.read_text(encoding="utf-8")
    assert unit_file.exists()
    assert "ExecStart=" in content
    assert "-m noisebot_server" in content
    assert f"WorkingDirectory={workdir}" in content
    assert "Restart=on-failure" in content
    assert any("daemon-reload" in call for call in calls)
    assert any("enable" in call for call in calls)


def test_server_healthcheck_is_server_owned(monkeypatch, tmp_path) -> None:
    health = importlib.import_module("noisebot_server.internal.service.healthcheck")

    health_file = tmp_path / "noisebot-server.health"
    monkeypatch.setattr(health, "HEALTHCHECK_FILE", health_file)

    health.write_healthy("ok")

    assert health_file.exists()
    assert health.is_healthy(max_age_s=60.0)
    assert "ok" in health_file.read_text(encoding="utf-8")

    health.write_unhealthy("teste")

    assert not health.is_healthy(max_age_s=60.0)

    health.remove_healthcheck()

    assert not health_file.exists()


def test_server_runtime_uses_noisebot_server_app() -> None:
    runtime = importlib.import_module("noisebot_server.runtime")
    app_module = importlib.import_module("noisebot_server.app")

    assert runtime.NoiseBotServer is app_module.NoiseBotServer


def test_server_app_no_longer_inherits_bridge_application() -> None:
    _ensure_bridgev2_path()

    app_module = importlib.import_module("noisebot_server.app")
    bridge_app = importlib.import_module("bridgev2.app")

    assert not issubclass(app_module.NoiseBotServer, bridge_app.Application)


def test_server_app_dry_run_suppresses_supervisor() -> None:
    app_module = importlib.import_module("noisebot_server.app")

    app = app_module.NoiseBotServer(
        _make_server_config(host="127.0.0.1", dry_run=True)
    )

    assert app._supervisor is None


def test_server_app_tcp_config_creates_supervisor() -> None:
    app_module = importlib.import_module("noisebot_server.app")

    app = app_module.NoiseBotServer(
        _make_server_config(host="127.0.0.1", dry_run=False)
    )

    assert app._supervisor is not None
    assert app._get_adapter() is None


def test_server_transport_exports_bridge_compatible_protocol() -> None:
    _ensure_bridgev2_path()

    server_protocol = importlib.import_module(
        "noisebot_server.internal.transport.protocol"
    )
    bridge_protocol = importlib.import_module("bridgev2.protocol.framing")
    bridge_messages = importlib.import_module("bridgev2.protocol.messages")

    payload = bridge_messages.encode_expr(3, 1500)

    assert server_protocol.encode_frame(bridge_messages.MSG_EXPR, payload) == (
        bridge_protocol.encode_frame(bridge_messages.MSG_EXPR, payload)
    )


def test_server_transport_protocol_is_server_owned() -> None:
    _ensure_bridgev2_path()

    server_protocol = importlib.import_module(
        "noisebot_server.internal.transport.protocol"
    )
    bridge_codec = importlib.import_module("bridgev2.protocol.codec")

    assert server_protocol.FrameDecoder is not bridge_codec.FrameDecoder


def test_server_transport_protocol_decodes_split_frames() -> None:
    protocol = importlib.import_module("noisebot_server.internal.transport.protocol")

    payload = protocol.encode_text_scroll("ola noise")
    frame = protocol.encode_frame(protocol.MSG_TEXT_SCROLL, payload)
    decoder = protocol.FrameDecoder()

    decoder.feed(frame[:3])

    assert decoder.frames() == []
    assert decoder.buffered_bytes == 3

    decoder.feed(frame[3:])

    assert decoder.frames() == [(protocol.MSG_TEXT_SCROLL, payload)]
    assert decoder.buffered_bytes == 0


def test_server_transport_protocol_discards_bad_crc() -> None:
    protocol = importlib.import_module("noisebot_server.internal.transport.protocol")
    frame = bytearray(protocol.encode_frame(protocol.MSG_VOLUME, protocol.encode_volume(50)))
    frame[-1] ^= 0xFF
    buf = bytearray(frame)

    assert protocol.decode_frames(buf) == []
    assert buf == bytearray()


def test_server_transport_factory_creates_tcp_transport() -> None:
    config_module = importlib.import_module("noisebot_server.config")
    factory_module = importlib.import_module(
        "noisebot_server.internal.transport.factory"
    )

    config = config_module.NoiseBotServerConfig(
        transport=config_module.TransportConfig(
            host="192.168.1.30",
            port=9000,
            uart=None,
            baudrate=1000000,
        ),
        llm=config_module.LlmConfig(
            provider=config_module.LlmProvider.NONE,
            model="none",
            timeout_s=10.0,
            max_output_tokens=256,
            max_reply_chars=180,
            ollama_base_url="http://127.0.0.1:11434",
            ollama_think=False,
            openai_key_configured=False,
            gemini_key_configured=False,
        ),
        pipeline_mode=config_module.PipelineMode.LOCAL_ONLY,
        stt=config_module.SttConfig(
            model="small",
            backend="faster",
            device="cpu",
            compute_type="int8",
        ),
        tts=config_module.TtsConfig(
            piper_executable="piper",
            piper_model="",
            cache_size=64,
            sample_rate=16000,
            target_peak=12000,
        ),
        audio=config_module.AudioConfig(
            chunk_samples=256,
            sample_rate=16000,
            min_transcribe_rms=140.0,
            min_transcribe_peak=1600,
            min_utterance_samples=8000,
            max_no_speech_prob=0.75,
            min_avg_logprob=-1.10,
            max_compression_ratio=2.60,
        ),
        reconnect=config_module.ReconnectConfig(
            delay_s=1.0,
            max_delay_s=30.0,
            connect_timeout_s=5.0,
        ),
        ops=config_module.OpsConfig(
            port=8765,
            token_configured=False,
        ),
        log_level=config_module.LogLevel.INFO,
        dry_run=True,
        replay_path=None,
    )

    transport = factory_module.create_transport_factory(config)()

    assert transport.description == "TCP 192.168.1.30:9000"


def test_server_transport_is_server_owned() -> None:
    _ensure_bridgev2_path()

    server_transport = importlib.import_module("noisebot_server.internal.transport")
    bridge_adapter = importlib.import_module("bridgev2.transport.adapter")
    bridge_tcp = importlib.import_module("bridgev2.transport.tcp")
    bridge_supervisor = importlib.import_module("bridgev2.transport.reconnect")

    assert server_transport.FirmwareAdapter is not bridge_adapter.FirmwareAdapter
    assert server_transport.TcpTransport is not bridge_tcp.TcpTransport
    assert (
        server_transport.ConnectionSupervisor
        is not bridge_supervisor.ConnectionSupervisor
    )


async def test_server_firmware_adapter_dispatches_voice_end_event() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    adapter_module = importlib.import_module("noisebot_server.internal.transport.adapter")
    protocol = importlib.import_module("noisebot_server.internal.transport.protocol")

    class DummyTransport:
        is_connected = True
        description = "dummy"

        async def connect(self) -> None:
            pass

        async def disconnect(self) -> None:
            pass

        async def send(self, data: bytes) -> None:
            pass

        async def recv(self, n: int = 4096) -> bytes:
            return b""

    bus = runtime.EventBus()
    queue = bus.subscribe(runtime.VoiceActivityEnd)
    adapter = adapter_module.FirmwareAdapter(DummyTransport(), bus)
    payload = struct.pack(
        "<I",
        protocol.NB_EVT_VOICE_ACTIVITY_END,
    ) + bytes([runtime.VoiceEndReason.TIMEOUT])

    await adapter._dispatch_rx(protocol.MSG_EVENT, payload)

    event = await asyncio.wait_for(queue.get(), timeout=0.1)
    assert event.reason == runtime.VoiceEndReason.TIMEOUT


def _server_loud_pcm(samples: int = 256, amplitude: int = 3200) -> bytes:
    return b"".join(struct.pack("<h", amplitude if i % 2 == 0 else -amplitude) for i in range(samples))


async def _simulate_server_voice_session(bus, runtime, chunks: int = 40) -> None:
    await bus.publish(runtime.WakeDetected())
    await asyncio.sleep(0)
    pcm = _server_loud_pcm()
    for seq in range(chunks):
        await bus.publish(runtime.AudioChunkIn(pcm=pcm, seq=seq))
    await asyncio.sleep(0)
    await bus.publish(runtime.VoiceActivityEnd())


async def _wait_until(predicate, timeout_s: float = 1.0) -> None:
    deadline = asyncio.get_running_loop().time() + timeout_s
    while not predicate():
        if asyncio.get_running_loop().time() >= deadline:
            raise AssertionError("condicao nao atendida dentro do timeout")
        await asyncio.sleep(0.01)


async def _drain_queue(queue: asyncio.Queue, duration_s: float = 0.05) -> list:
    items = []
    deadline = asyncio.get_running_loop().time() + duration_s
    while True:
        remaining = deadline - asyncio.get_running_loop().time()
        if remaining <= 0:
            return items
        try:
            items.append(await asyncio.wait_for(queue.get(), timeout=remaining))
        except asyncio.TimeoutError:
            return items


async def test_server_empty_wake_prompt_is_one_shot_until_real_speech() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )

    class MockSequenceStt:
        def __init__(self, results) -> None:
            self._results = list(results)
            self.finalize_calls = 0

        async def initialize(self) -> None:
            pass

        def feed(self, pcm: bytes) -> None:
            pass

        async def finalize(self, full_pcm: bytes, turn_id: int):
            self.finalize_calls += 1
            text, quality = self._results.pop(0)
            return runtime.FinalTranscript(turn_id=turn_id, text=text, quality=quality)

        async def close(self) -> None:
            pass

    bus = runtime.EventBus(default_maxsize=512)
    stt = MockSequenceStt([
        ("", runtime.TranscriptQuality.EMPTY),
        ("", runtime.TranscriptQuality.EMPTY),
        ("olá", runtime.TranscriptQuality.GOOD),
        ("", runtime.TranscriptQuality.EMPTY),
    ])
    orchestrator = orchestrator_module.Orchestrator(
        bus,
        _make_server_config(),
        get_adapter=lambda: None,
        stt_provider=stt,
    )
    intents = bus.subscribe(runtime.IntentResolved)
    speech_done = bus.subscribe(runtime.SpeechDone)
    task = asyncio.create_task(orchestrator.run())

    try:
        await _simulate_server_voice_session(bus, runtime)
        first_intent = await asyncio.wait_for(intents.get(), timeout=1.0)
        await asyncio.wait_for(speech_done.get(), timeout=1.0)
        await _wait_until(lambda: orchestrator._session is None)
        assert first_intent.intent_name == "local_empty_wake_prompt"

        await _simulate_server_voice_session(bus, runtime)
        await _wait_until(
            lambda: stt.finalize_calls >= 2 and orchestrator._session is None
        )
        assert await _drain_queue(intents) == []

        await _simulate_server_voice_session(bus, runtime)
        greeting_intent = await asyncio.wait_for(intents.get(), timeout=1.0)
        await asyncio.wait_for(speech_done.get(), timeout=1.0)
        await _wait_until(lambda: orchestrator._session is None)
        assert greeting_intent.intent_name == "local_greeting"

        await _simulate_server_voice_session(bus, runtime)
        second_prompt = await asyncio.wait_for(intents.get(), timeout=1.0)
        await asyncio.wait_for(speech_done.get(), timeout=1.0)
        await _wait_until(lambda: orchestrator._session is None)
        assert second_prompt.intent_name == "local_empty_wake_prompt"
    finally:
        await orchestrator.shutdown()
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)


def test_server_transport_factory_creates_uart_transport() -> None:
    factory_module = importlib.import_module(
        "noisebot_server.internal.transport.factory"
    )
    config = _make_server_config(uart="COM9")

    transport = factory_module.create_transport_factory(config)()

    assert transport.description == "UART COM9@1000000"


def test_server_connection_supervisor_backoff_caps() -> None:
    transport = importlib.import_module("noisebot_server.internal.transport")
    config = _make_server_config()
    supervisor = transport.ConnectionSupervisor(
        transport_factory=lambda: transport.TcpTransport("127.0.0.1"),
        bus=object(),
        reconnect=config.reconnect,
    )

    assert supervisor._next_delay(0.2) == 0.2


def test_server_ops_status_store_is_server_owned() -> None:
    _ensure_bridgev2_path()

    server_ops = importlib.import_module("noisebot_server.internal.ops")
    bridge_status = importlib.import_module("bridgev2.ops.status_store")

    assert server_ops.StatusStore is not bridge_status.StatusStore

    store = server_ops.StatusStore()
    bridge_store = bridge_status.StatusStore()
    store.record_turn(
        11,
        "llm",
        transcript="minha chave sk-abcdef1234567890",
        reply="ok",
        route="llm",
    )
    bridge_store.record_turn(
        11,
        "llm",
        transcript="minha chave sk-abcdef1234567890",
        reply="ok",
        route="llm",
    )

    assert "sk-abcdef1234567890" not in store.last_transcript
    assert "<redacted>" in store.last_transcript
    assert store.last_transcript == bridge_store.last_transcript


def test_server_ops_exports_bridge_compatible_schemas() -> None:
    _ensure_bridgev2_path()

    server_schemas = importlib.import_module("noisebot_server.internal.ops.schemas")
    bridge_schemas = importlib.import_module("bridgev2.ops.schemas")

    assert server_schemas.ok_response("feito") == bridge_schemas.ok_response("feito")
    assert server_schemas.error_response("falha", 503) == (
        bridge_schemas.error_response("falha", 503)
    )


def test_server_ops_config_controller_is_server_owned() -> None:
    _ensure_bridgev2_path()

    server_config = importlib.import_module("noisebot_server.internal.ops.config")
    bridge_config = importlib.import_module("bridgev2.ops.config_controller")

    assert server_config.ConfigController is not bridge_config.ConfigController
    assert server_config.PROVIDER_CATALOG == bridge_config.PROVIDER_CATALOG
    assert server_config.VALID_MODES == bridge_config.VALID_MODES
    assert "ollama" in server_config.PROVIDER_CATALOG
    assert "local_only" in server_config.VALID_MODES


def test_server_app_state_persists_routine_and_basic_settings(tmp_path) -> None:
    app_state = importlib.import_module("noisebot_server.internal.ops.app_state")
    state_path = tmp_path / "app_state.json"

    store = app_state.AppStateStore(state_path)
    timer = store.create_agenda_item("timer", {"title": "Cafe", "duration_min": 5})
    settings = store.update_basic_settings(
        {
            "volume": 88,
            "display_brightness": 42,
            "led_brightness": 17,
            "night_mode": True,
        }
    )

    assert timer["kind"] == "timer"
    assert timer["status"] == "ativo"
    assert settings["volume"] == 88
    assert settings["display_brightness"] == 42
    assert settings["night_mode"] is True

    reloaded = app_state.AppStateStore(state_path)
    snapshot = reloaded.snapshot()

    assert snapshot["routine"]["summary"]["timers"] == 1
    assert snapshot["routine"]["items"][0]["title"] == "Cafe"
    assert snapshot["settings"]["volume"] == 88


def test_server_app_state_updates_and_deletes_agenda_items(tmp_path) -> None:
    app_state = importlib.import_module("noisebot_server.internal.ops.app_state")
    store = app_state.AppStateStore(tmp_path / "app_state.json")

    alarm = store.create_agenda_item("alarm", {"title": "Acordar", "time": "07:00"})
    updated = store.update_agenda_item(alarm["id"], {"enabled": False})

    assert updated is not None
    assert updated["enabled"] is False
    assert updated["status"] == "desligado"
    assert store.list_agenda()["summary"]["alarms"] == 0
    assert store.delete_agenda_item(alarm["id"]) is True
    assert store.delete_agenda_item(alarm["id"]) is False


def test_server_app_state_maps_alarm_repeat_to_firmware_mask(tmp_path) -> None:
    app_state = importlib.import_module("noisebot_server.internal.ops.app_state")
    store = app_state.AppStateStore(tmp_path / "app_state.json")

    daily = store.create_agenda_item("alarm", {"title": "Todo dia", "repeat": "diário"})
    weekdays = store.create_agenda_item("alarm", {"title": "Trabalho", "repeat": "dias úteis"})
    weekend = store.create_agenda_item("alarm", {"title": "Folga", "repeat": "fim de semana"})
    updated = store.update_agenda_item(daily["id"], {"repeat": "uma vez", "time": "08:15"})

    assert daily["weekdays_mask"] == 0x7F
    assert weekdays["weekdays_mask"] == 0x3E
    assert weekend["weekdays_mask"] == 0x41
    assert updated is not None
    assert updated["weekdays_mask"] == 0
    assert updated["detail"] == "uma vez, 08:15"


def test_server_app_state_imports_firmware_agenda_without_duplicates(tmp_path) -> None:
    app_state = importlib.import_module("noisebot_server.internal.ops.app_state")
    store = app_state.AppStateStore(tmp_path / "app_state.json")
    store.create_agenda_item("alarm", {"title": "Remedio", "time": "08:05"})

    imported = store.import_firmware_agenda({
        "alarms": [
            {
                "id": 2,
                "label": "Remedio",
                "hour": 8,
                "minute": 5,
                "weekdays_mask": 0,
                "enabled": True,
            }
        ],
        "timers": [
            {
                "id": 1,
                "label": "Cafe",
                "duration_ms": 300000,
                "remaining_ms": 120000,
            }
        ],
        "reminders": [],
    })
    imported_again = store.import_firmware_agenda({
        "alarms": [
            {
                "id": 2,
                "label": "Remedio",
                "hour": 8,
                "minute": 5,
                "weekdays_mask": 0,
                "enabled": False,
            }
        ],
        "timers": [],
        "reminders": [],
    })

    agenda = store.list_agenda()

    assert imported == 2
    assert imported_again == 1
    assert agenda["summary"]["alarms"] == 0
    assert agenda["summary"]["timers"] == 0
    assert len(agenda["items"]) == 1
    assert agenda["items"][0]["id"] == "fw_alarm_2"
    assert agenda["items"][0]["source"] == "firmware"
    assert agenda["items"][0]["firmware_id"] == 2
    assert agenda["items"][0]["time"] == "08:05"
    assert agenda["items"][0]["enabled"] is False

    assert store.import_firmware_agenda({"alarms": [], "timers": [], "reminders": []}) == 0
    assert store.list_agenda()["items"] == []


def test_server_ops_serves_app_dist_when_available(tmp_path, monkeypatch) -> None:
    http = importlib.import_module("noisebot_server.internal.ops.http")
    dist = tmp_path / "dist"
    dist.mkdir()
    (dist / "index.html").write_text("<div>NoiseBot App</div>", encoding="utf-8")

    monkeypatch.setenv("NOISEBOT_APP_DIST", str(dist))

    assert http._find_app_dist() == dist.resolve()


def test_server_agenda_payload_recreates_edited_alarm() -> None:
    http = importlib.import_module("noisebot_server.internal.ops.http")

    payload = http._agenda_session_payload(
        {
            "kind": "alarm",
            "title": "Remedio",
            "time": "08:15",
            "weekdays_mask": 0x3E,
            "enabled": True,
            "firmware_id": 2,
        },
        "recreate",
    )

    assert isinstance(payload, list)
    assert payload[0]["action"] == "alarm_cancel"
    assert payload[0]["id"] == 2
    assert payload[1]["action"] == "alarm_create"
    assert payload[1]["weekdays_mask"] == 0x3E


def test_server_agent_orchestrator_is_server_owned() -> None:
    _ensure_bridgev2_path()

    server_agent = importlib.import_module("noisebot_server.internal.agent")
    bridge_orchestrator = importlib.import_module("bridgev2.runtime.orchestrator")

    assert server_agent.Orchestrator is not bridge_orchestrator.Orchestrator


def test_server_agent_runtime_is_server_owned() -> None:
    _ensure_bridgev2_path()

    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    bridge_bus = importlib.import_module("bridgev2.runtime.bus")
    bridge_events = importlib.import_module("bridgev2.runtime.events")
    bridge_turn = importlib.import_module("bridgev2.runtime.turn_manager")

    assert runtime.EventBus is not bridge_bus.EventBus
    assert runtime.VoiceEndReason.SILENCE.value == bridge_events.VoiceEndReason.SILENCE.value
    assert runtime.TurnState.IDLE.name == bridge_turn.TurnState.IDLE.name


def test_server_agent_turn_manager_keeps_transition_rules() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    manager = runtime.TurnManager()

    manager.transition(runtime.TurnState.LISTENING, turn_id=42)
    manager.transition(runtime.TurnState.COMMITTING_TURN)
    manager.transition(runtime.TurnState.THINKING)

    assert manager.current_turn_id == 42
    assert manager.can_interrupt is True
    assert manager.try_transition(runtime.TurnState.COMMITTING_TURN) is False
    assert manager.state == runtime.TurnState.THINKING


def test_server_agent_local_intent_matches_time() -> None:
    _ensure_bridgev2_path()

    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match("que horas sao", turn_id=44)

    assert result.intent_name == "local_time"
    assert result.reply_text


def test_server_agent_llm_and_intents_are_server_owned() -> None:
    _ensure_bridgev2_path()

    server_agent = importlib.import_module("noisebot_server.internal.agent")
    server_llm = importlib.import_module("noisebot_server.internal.agent.llm")
    bridge_llm = importlib.import_module("bridgev2.llm.base")
    bridge_intents = importlib.import_module("bridgev2.llm.local_intent")

    assert server_llm.StreamingLLMProvider is not bridge_llm.StreamingLLMProvider
    assert server_agent.LocalIntentProvider is not bridge_intents.LocalIntentProvider


def test_server_agent_stt_tts_are_server_owned() -> None:
    _ensure_bridgev2_path()

    server_stt = importlib.import_module("noisebot_server.internal.agent.stt")
    bridge_stt = importlib.import_module("bridgev2.stt.base")
    server_tts = importlib.import_module("noisebot_server.internal.agent.tts")
    bridge_tts = importlib.import_module("bridgev2.tts.base")

    assert server_stt.STTProvider is not bridge_stt.STTProvider
    assert server_tts.TTSProvider is not bridge_tts.TTSProvider


def test_server_agent_sentencizer_keeps_bridge_behavior() -> None:
    _ensure_bridgev2_path()

    agent = importlib.import_module("noisebot_server.internal.agent")
    sentencizer = agent.Sentencizer()

    sentences = list(sentencizer.feed("Ola. Tudo bem?")) + list(sentencizer.flush())

    assert sentences == ["Ola. Tudo bem?"]


def test_server_vision_is_server_owned() -> None:
    _ensure_bridgev2_path()

    server_vision = importlib.import_module("noisebot_server.internal.vision")
    bridge_vision = importlib.import_module("bridgev2.vision")

    assert server_vision.VisionClient is not bridge_vision.VisionClient
    assert server_vision.VisionObservation is not bridge_vision.VisionObservation
    assert server_vision.FaceBox is not bridge_vision.FaceBox


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


def test_server_app_contract_exposes_only_server_paths() -> None:
    api = importlib.import_module("noisebot_server.api")

    endpoints = api.default_app_contract()

    assert endpoints
    assert all(endpoint.path.startswith("/") for endpoint in endpoints)
    assert all(not endpoint.path.startswith("http://") for endpoint in endpoints)
    assert all(not endpoint.path.startswith("https://") for endpoint in endpoints)


def test_server_app_contract_tracks_implemented_endpoints() -> None:
    api = importlib.import_module("noisebot_server.api")

    implemented = api.implemented_endpoints()
    paths = {(endpoint.method, endpoint.path) for endpoint in implemented}

    assert ("GET", "/health") in paths
    assert ("GET", "/ai/status") in paths
    assert ("POST", "/debug/transcript") in paths
    assert all(endpoint.implemented for endpoint in implemented)


def test_server_app_contract_reserves_future_domains() -> None:
    api = importlib.import_module("noisebot_server.api")

    domains = {endpoint.domain for endpoint in api.default_app_contract()}

    assert {"ops", "vision", "agent", "device", "agenda"}.issubset(domains)
