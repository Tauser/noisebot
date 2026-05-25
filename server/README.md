# NoiseBot Server

Local companion server for NoiseBot.

This follows the useful part of StackChan's server shape, but the role is
different: NoiseBot keeps the ESP32 firmware small and moves heavy operation
surfaces here.

Responsibilities:

- Bridge transport to firmware over TCP/UART.
- Local ops API for diagnostics and dashboard data.
- AI provider orchestration for STT, LLM and TTS.
- Vision orchestration that can call firmware snapshots and run heavier logic on
  the host.
- Local-only configuration surfaces. Secrets stay outside firmware and outside
  committed files.

Initial mapping:

```text
server/
├── api/        # Request/response contracts exposed to app/dashboard
├── internal/   # Controllers, services, models, transport adapters
├── manifest/   # Config, deployment and local runtime manifests
└── resource/   # Public/static/runtime resources owned by the server
```

The existing implementation is currently in `bridge_v2/`. New product-facing
server work should be added here gradually, with adapters into `bridge_v2/`
instead of copying firmware dashboard code back into the ESP32.

## Migration status

Phase 1 is intentionally conservative:

- `noisebot_server` is now an importable package under `server/`.
- `noisebot-server` delegates to the existing `bridgev2` entrypoint.
- `NoiseBotServer` is a facade over `bridgev2.app.Application`.
- No firmware behavior is changed by this phase.
- Runtime code remains owned by `bridge_v2` until each server module has parity
  tests.

Next phases should move responsibilities one boundary at a time:

1. `internal/transport`: firmware connection, framing and reconnect.
2. `internal/ops`: health, metrics, debug and config API.
3. `internal/agent`: STT, local intents, LLM providers and TTS.
4. `internal/vision`: snapshot orchestration and higher-level observations.

## Transport boundary

`noisebot_server.internal.transport` is the first real server boundary. It
currently delegates to `bridge_v2` for byte-compatible behavior, but new code
should import transport and protocol symbols from the server package:

```python
from noisebot_server.internal.transport import TcpTransport
from noisebot_server.internal.transport.protocol import encode_frame
```

This keeps the migration reversible and avoids a risky one-shot move of TCP,
UART, reconnect, handshake and framing code.

## Ops boundary

`noisebot_server.internal.ops` is the server-owned import boundary for local
operation surfaces: health, status, metrics, debug, config and token checks.
It delegates to `bridge_v2` in this phase so the existing dashboard/API remains
unchanged while future app-facing APIs move into the server package.

```python
from noisebot_server.internal.ops import OpsHttpServer, StatusStore
```

## Agent boundary

`noisebot_server.internal.agent` is the server-owned import boundary for the
voice/agent pipeline: conversation runtime, local intents, STT, LLM providers,
TTS and orchestration. It delegates to `bridge_v2` in this phase so voice,
barge-in, local LLM configuration and Piper playback behavior stay unchanged.

```python
from noisebot_server.internal.agent import LocalIntentProvider, Orchestrator
```

## Vision boundary

`noisebot_server.internal.vision` is the server-owned import boundary for camera
work: firmware snapshot/observe calls, local image analysis and future
owner-recognition services. The firmware should stay responsible for capture;
heavier interpretation belongs here.

```python
from noisebot_server.internal.vision import VisionClient, analyze_jpeg
```

## App boundary

The external app/dashboard should consume a server-owned contract instead of
calling firmware endpoints directly. The current contract is defined in:

```python
from noisebot_server.api import default_app_contract, implemented_endpoints
```

Endpoints marked as implemented are served by the current bridge-backed runtime.
Reserved endpoints describe the next API surface for app, vision, agent, device
and agenda work.
