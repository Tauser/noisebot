"""Helpers compartilhados extraidos de test_server_facade.py (SF-05)."""

from __future__ import annotations
import asyncio
import importlib
import io
import json
import logging
import math
import struct
from pathlib import Path
from urllib.error import HTTPError
import pytest


def _make_server_config(
    *,
    host: str | None = None,
    port: int = 9000,
    uart: str | None = None,
    dry_run: bool = True,
    piper_model: str = "",
    max_utterance_samples: int = 160000,
    default_codec: str = "pcm16",
    followup_enabled: bool = False,
    followup_window_ms: int = 8000,
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
            temperature=0.7,
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
            default_codec=default_codec,
            min_transcribe_rms=140.0,
            min_transcribe_peak=1600,
            min_utterance_samples=8000,
            max_utterance_samples=max_utterance_samples,
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
        conversation=config_module.ConversationConfig(
            followup_enabled=followup_enabled,
            followup_window_ms=followup_window_ms,
        ),
        log_level=config_module.LogLevel.INFO,
        dry_run=dry_run,
        replay_path=None,
    )

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
