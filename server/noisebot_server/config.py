"""Server configuration facade.

The typed config still comes from ``bridge_v2`` in this phase. Keeping this
module lets server runtime code stop importing bridge internals directly.
"""

from __future__ import annotations

from ._compat import ensure_bridgev2_path

ensure_bridgev2_path()

from bridgev2.config import (
    AudioConfig,
    BridgeV2Config,
    LlmConfig,
    LlmProvider,
    LogLevel,
    OpsConfig,
    PipelineMode,
    ReconnectConfig,
    SttConfig,
    TransportConfig,
    TtsConfig,
    find_env_file,
    load_config,
    load_env_file,
)

__all__ = [
    "AudioConfig",
    "BridgeV2Config",
    "LlmConfig",
    "LlmProvider",
    "LogLevel",
    "OpsConfig",
    "PipelineMode",
    "ReconnectConfig",
    "SttConfig",
    "TransportConfig",
    "TtsConfig",
    "find_env_file",
    "load_config",
    "load_env_file",
]
