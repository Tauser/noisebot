"""Testes de bridgev2.config — Fase 1 critério: lint + testes passam."""
from __future__ import annotations

import os
import pytest

from bridgev2.config import (
    BridgeV2Config,
    LlmProvider,
    PipelineMode,
    LogLevel,
    load_config,
    load_env_file,
    find_env_file,
)


class TestLoadConfig:
    def test_defaults(self, monkeypatch):
        """load_config() sem variáveis de ambiente retorna valores padrão sensatos."""
        # Limpa variáveis relevantes
        keys = [
            "NOISEBOT_HOST", "NOISEBOT_PORT", "NOISEBOT_UART",
            "NOISEBOT_LLM_PROVIDER", "NOISEBOT_LLM_MODEL", "NOISEBOT_PIPELINE_MODE",
            "NOISEBOT_OLLAMA_BASE_URL", "NOISEBOT_OLLAMA_THINK",
            "OPENAI_API_KEY", "GEMINI_API_KEY",
            "NOISEBOT_WHISPER_MODEL", "NOISEBOT_DRY_RUN", "NOISEBOT_LOG_LEVEL",
        ]
        for k in keys:
            monkeypatch.delenv(k, raising=False)

        cfg = load_config(env_path="/nonexistent/.env")

        assert cfg.transport.port == 9000
        assert cfg.transport.host is None
        assert cfg.llm.provider == LlmProvider.OLLAMA
        assert cfg.llm.model == "qwen3.5:9b"
        assert cfg.pipeline_mode == PipelineMode.NORMAL
        assert cfg.stt.model == "small"
        assert cfg.dry_run is False
        assert cfg.log_level == LogLevel.INFO

    def test_host_from_env(self, monkeypatch):
        monkeypatch.setenv("NOISEBOT_HOST", "192.168.1.42")
        monkeypatch.setenv("NOISEBOT_PORT", "9001")
        cfg = load_config(env_path="/nonexistent/.env")
        assert cfg.transport.host == "192.168.1.42"
        assert cfg.transport.port == 9001
        assert cfg.transport.use_tcp is True

    def test_uart_mode(self, monkeypatch):
        monkeypatch.delenv("NOISEBOT_HOST", raising=False)
        monkeypatch.setenv("NOISEBOT_UART", "COM3")
        cfg = load_config(env_path="/nonexistent/.env")
        assert cfg.transport.uart == "COM3"
        assert cfg.transport.use_tcp is False

    def test_pipeline_mode_local_only(self, monkeypatch):
        monkeypatch.setenv("NOISEBOT_PIPELINE_MODE", "local_only")
        cfg = load_config(env_path="/nonexistent/.env")
        assert cfg.pipeline_mode == PipelineMode.LOCAL_ONLY

    def test_pipeline_mode_invalid_falls_back(self, monkeypatch):
        monkeypatch.setenv("NOISEBOT_PIPELINE_MODE", "invalid_value")
        cfg = load_config(env_path="/nonexistent/.env")
        assert cfg.pipeline_mode == PipelineMode.NORMAL

    def test_llm_provider_gemini(self, monkeypatch):
        monkeypatch.setenv("NOISEBOT_LLM_PROVIDER", "gemini")
        monkeypatch.delenv("NOISEBOT_LLM_MODEL", raising=False)
        cfg = load_config(env_path="/nonexistent/.env")
        assert cfg.llm.provider == LlmProvider.GEMINI
        assert cfg.llm.model == "gemini-2.5-flash"

    def test_llm_provider_openai_uses_openai_default_model(self, monkeypatch):
        monkeypatch.setenv("NOISEBOT_LLM_PROVIDER", "openai")
        monkeypatch.delenv("NOISEBOT_LLM_MODEL", raising=False)
        cfg = load_config(env_path="/nonexistent/.env")
        assert cfg.llm.provider == LlmProvider.OPENAI
        assert cfg.llm.model == "gpt-4o-mini"

    def test_llm_provider_ollama(self, monkeypatch):
        monkeypatch.setenv("NOISEBOT_LLM_PROVIDER", "ollama")
        monkeypatch.setenv("NOISEBOT_LLM_MODEL", "qwen2.5:7b")
        monkeypatch.setenv("NOISEBOT_OLLAMA_BASE_URL", "http://localhost:11434")
        cfg = load_config(env_path="/nonexistent/.env")
        assert cfg.llm.provider == LlmProvider.OLLAMA
        assert cfg.llm.model == "qwen2.5:7b"
        assert cfg.llm.ollama_base_url == "http://localhost:11434"
        assert cfg.llm.ollama_think is False

    def test_llm_provider_ollama_think_opt_in(self, monkeypatch):
        monkeypatch.setenv("NOISEBOT_LLM_PROVIDER", "ollama")
        monkeypatch.setenv("NOISEBOT_OLLAMA_THINK", "true")
        cfg = load_config(env_path="/nonexistent/.env")
        assert cfg.llm.ollama_think is True

    def test_api_key_configured_flag(self, monkeypatch):
        monkeypatch.setenv("OPENAI_API_KEY", "sk-test-secret-key")
        monkeypatch.delenv("GEMINI_API_KEY", raising=False)
        cfg = load_config(env_path="/nonexistent/.env")
        assert cfg.llm.openai_key_configured is True
        assert cfg.llm.gemini_key_configured is False

    def test_api_key_never_in_safe_dict(self, monkeypatch):
        monkeypatch.setenv("OPENAI_API_KEY", "sk-this-must-never-appear")
        cfg = load_config(env_path="/nonexistent/.env")
        safe = cfg.safe_dict()
        # Serializa como string e verifica que a chave não aparece
        safe_str = str(safe)
        assert "sk-this-must-never-appear" not in safe_str

    def test_dry_run(self, monkeypatch):
        monkeypatch.setenv("NOISEBOT_DRY_RUN", "true")
        cfg = load_config(env_path="/nonexistent/.env")
        assert cfg.dry_run is True

    def test_log_level_debug(self, monkeypatch):
        monkeypatch.setenv("NOISEBOT_LOG_LEVEL", "DEBUG")
        cfg = load_config(env_path="/nonexistent/.env")
        assert cfg.log_level == LogLevel.DEBUG

    def test_ops_token_configured(self, monkeypatch):
        monkeypatch.setenv("NOISEBOT_OPS_TOKEN", "my-secret-token")
        cfg = load_config(env_path="/nonexistent/.env")
        assert cfg.ops.token_configured is True
        # token não aparece em safe_dict
        assert "my-secret-token" not in str(cfg.safe_dict())

    def test_ops_token_not_configured(self, monkeypatch):
        monkeypatch.delenv("NOISEBOT_OPS_TOKEN", raising=False)
        cfg = load_config(env_path="/nonexistent/.env")
        assert cfg.ops.token_configured is False

    def test_config_is_frozen(self, monkeypatch):
        cfg = load_config(env_path="/nonexistent/.env")
        with pytest.raises((AttributeError, TypeError)):
            cfg.dry_run = True  # type: ignore[misc]

    def test_safe_dict_keys(self, monkeypatch):
        cfg = load_config(env_path="/nonexistent/.env")
        d = cfg.safe_dict()
        assert "transport" in d
        assert "llm" in d
        assert "pipeline_mode" in d
        assert "stt" in d
        assert "tts" in d
        assert "audio" in d
        assert "reconnect" in d
        assert "ops" in d
        # Nunca deve ter "api_key" com valor de chave
        assert "openai_key" not in d
        assert "gemini_key" not in d


class TestLoadEnvFile:
    def test_reads_env_file(self, tmp_path, monkeypatch):
        env_file = tmp_path / ".env"
        env_file.write_text("NOISEBOT_PORT=8888\n# comentário\nNOISEBOT_DRY_RUN=true\n")
        monkeypatch.delenv("NOISEBOT_PORT", raising=False)
        monkeypatch.delenv("NOISEBOT_DRY_RUN", raising=False)

        load_env_file(env_file)

        assert os.environ.get("NOISEBOT_PORT") == "8888"
        assert os.environ.get("NOISEBOT_DRY_RUN") == "true"

    def test_does_not_overwrite_existing(self, tmp_path, monkeypatch):
        env_file = tmp_path / ".env"
        env_file.write_text("NOISEBOT_PORT=7777\n")
        monkeypatch.setenv("NOISEBOT_PORT", "9999")

        load_env_file(env_file)

        assert os.environ.get("NOISEBOT_PORT") == "9999"  # não sobrescrito

    def test_env_file_fills_empty_existing_value(self, tmp_path, monkeypatch):
        env_file = tmp_path / ".env"
        env_file.write_text("NOISEBOT_PIPER_MODEL=D:/models/voice.onnx\n")
        monkeypatch.setenv("NOISEBOT_PIPER_MODEL", "")

        load_env_file(env_file)

        assert os.environ.get("NOISEBOT_PIPER_MODEL") == "D:/models/voice.onnx"

    def test_default_loader_finds_repo_bridge_env(self, tmp_path, monkeypatch):
        import bridgev2.config as config_module

        fake_pkg = tmp_path / "pkg" / "bridgev2"
        fake_pkg.mkdir(parents=True)
        monkeypatch.setattr(config_module, "__file__", str(fake_pkg / "config.py"))
        monkeypatch.chdir(tmp_path)
        monkeypatch.delenv("NOISEBOT_PIPER_MODEL", raising=False)
        env_dir = tmp_path / "bridge_v2"
        env_dir.mkdir()
        (env_dir / ".env").write_text("NOISEBOT_PIPER_MODEL=D:/models/fallback.onnx\n")

        load_env_file()

        assert os.environ.get("NOISEBOT_PIPER_MODEL") == "D:/models/fallback.onnx"

    def test_find_env_file_uses_explicit_path(self, tmp_path):
        env_file = tmp_path / ".env"
        env_file.write_text("NOISEBOT_PIPER_MODEL=x\n")

        assert find_env_file(env_file) == env_file.resolve()

    def test_nonexistent_file_ok(self):
        load_env_file("/nonexistent/.env")  # não lança exceção

    def test_strips_quotes(self, tmp_path, monkeypatch):
        env_file = tmp_path / ".env"
        env_file.write_text('NOISEBOT_LLM_MODEL="gpt-4o"\n')
        monkeypatch.delenv("NOISEBOT_LLM_MODEL", raising=False)
        load_env_file(env_file)
        assert os.environ.get("NOISEBOT_LLM_MODEL") == "gpt-4o"
