from __future__ import annotations

import importlib


def test_bridgev2_compat_path_allows_application_import() -> None:
    compat = importlib.import_module("noisebot_server._compat")
    compat.ensure_bridgev2_path()

    app_module = importlib.import_module("noisebot_server.app")

    assert hasattr(app_module, "NoiseBotServer")


def test_server_cli_delegates_to_bridgev2_entrypoint() -> None:
    compat = importlib.import_module("noisebot_server._compat")
    compat.ensure_bridgev2_path()

    cli_module = importlib.import_module("noisebot_server.__main__")

    assert callable(cli_module.main)


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
