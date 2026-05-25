from __future__ import annotations

import asyncio
import importlib
import struct


def _make_server_config(
    *,
    host: str | None = None,
    port: int = 9000,
    uart: str | None = None,
    dry_run: bool = True,
    piper_model: str = "",
):
    config_module = importlib.import_module("noisebot_server.config")

    return config_module.BridgeV2Config(
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


def test_bridgev2_compat_path_allows_application_import() -> None:
    compat = importlib.import_module("noisebot_server._compat")
    compat.ensure_bridgev2_path()

    app_module = importlib.import_module("noisebot_server.app")

    assert hasattr(app_module, "NoiseBotServer")


def test_server_entrypoint_exposes_server_cli() -> None:
    compat = importlib.import_module("noisebot_server._compat")
    compat.ensure_bridgev2_path()

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
    compat = importlib.import_module("noisebot_server._compat")
    compat.ensure_bridgev2_path()

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
    compat = importlib.import_module("noisebot_server._compat")
    compat.ensure_bridgev2_path()

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
    compat = importlib.import_module("noisebot_server._compat")
    compat.ensure_bridgev2_path()

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
    compat = importlib.import_module("noisebot_server._compat")
    compat.ensure_bridgev2_path()

    config_module = importlib.import_module("bridgev2.config")
    factory_module = importlib.import_module(
        "noisebot_server.internal.transport.factory"
    )

    config = config_module.BridgeV2Config(
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
    compat = importlib.import_module("noisebot_server._compat")
    compat.ensure_bridgev2_path()

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


def test_server_ops_exports_bridge_compatible_status_store() -> None:
    compat = importlib.import_module("noisebot_server._compat")
    compat.ensure_bridgev2_path()

    server_ops = importlib.import_module("noisebot_server.internal.ops")
    bridge_status = importlib.import_module("bridgev2.ops.status_store")

    assert server_ops.StatusStore is bridge_status.StatusStore

    store = server_ops.StatusStore()
    store.record_turn(
        11,
        "llm",
        transcript="minha chave sk-abcdef1234567890",
        reply="ok",
        route="llm",
    )

    assert "sk-abcdef1234567890" not in store.last_transcript
    assert "<redacted>" in store.last_transcript


def test_server_ops_exports_bridge_compatible_schemas() -> None:
    compat = importlib.import_module("noisebot_server._compat")
    compat.ensure_bridgev2_path()

    server_schemas = importlib.import_module("noisebot_server.internal.ops.schemas")
    bridge_schemas = importlib.import_module("bridgev2.ops.schemas")

    assert server_schemas.ok_response("feito") == bridge_schemas.ok_response("feito")
    assert server_schemas.error_response("falha", 503) == (
        bridge_schemas.error_response("falha", 503)
    )


def test_server_ops_config_controller_validates_like_bridge() -> None:
    compat = importlib.import_module("noisebot_server._compat")
    compat.ensure_bridgev2_path()

    server_config = importlib.import_module("noisebot_server.internal.ops.config")
    bridge_config = importlib.import_module("bridgev2.ops.config_controller")

    assert server_config.ConfigController is bridge_config.ConfigController
    assert "ollama" in server_config.PROVIDER_CATALOG
    assert "local_only" in server_config.VALID_MODES


def test_server_agent_exports_bridge_orchestrator_runtime() -> None:
    compat = importlib.import_module("noisebot_server._compat")
    compat.ensure_bridgev2_path()

    server_agent = importlib.import_module("noisebot_server.internal.agent")
    bridge_orchestrator = importlib.import_module("bridgev2.runtime.orchestrator")
    bridge_bus = importlib.import_module("bridgev2.runtime.bus")

    assert server_agent.Orchestrator is bridge_orchestrator.Orchestrator
    assert server_agent.EventBus is bridge_bus.EventBus


def test_server_agent_local_intent_matches_time() -> None:
    compat = importlib.import_module("noisebot_server._compat")
    compat.ensure_bridgev2_path()

    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match("que horas sao", turn_id=44)

    assert result.intent_name == "local_time"
    assert result.reply_text


def test_server_agent_exports_provider_boundaries() -> None:
    compat = importlib.import_module("noisebot_server._compat")
    compat.ensure_bridgev2_path()

    server_llm = importlib.import_module("noisebot_server.internal.agent.llm")
    bridge_llm = importlib.import_module("bridgev2.llm.base")
    server_stt = importlib.import_module("noisebot_server.internal.agent.stt")
    bridge_stt = importlib.import_module("bridgev2.stt.base")
    server_tts = importlib.import_module("noisebot_server.internal.agent.tts")
    bridge_tts = importlib.import_module("bridgev2.tts.base")

    assert server_llm.StreamingLLMProvider is bridge_llm.StreamingLLMProvider
    assert server_stt.STTProvider is bridge_stt.STTProvider
    assert server_tts.TTSProvider is bridge_tts.TTSProvider


def test_server_agent_sentencizer_keeps_bridge_behavior() -> None:
    compat = importlib.import_module("noisebot_server._compat")
    compat.ensure_bridgev2_path()

    agent = importlib.import_module("noisebot_server.internal.agent")
    sentencizer = agent.Sentencizer()

    sentences = list(sentencizer.feed("Ola. Tudo bem?")) + list(sentencizer.flush())

    assert sentences == ["Ola. Tudo bem?"]


def test_server_vision_exports_bridge_client_and_analysis() -> None:
    compat = importlib.import_module("noisebot_server._compat")
    compat.ensure_bridgev2_path()

    server_vision = importlib.import_module("noisebot_server.internal.vision")
    bridge_vision = importlib.import_module("bridgev2.vision")

    assert server_vision.VisionClient is bridge_vision.VisionClient
    assert server_vision.VisionObservation is bridge_vision.VisionObservation
    assert server_vision.analyze_jpeg is bridge_vision.analyze_jpeg


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
