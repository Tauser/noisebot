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

`noisebot_server` is the canonical, self-contained server implementation.
`bridge` and `bridge_v2` are legacy packages from earlier iterations and are no
longer part of the runtime path — new server work belongs entirely under
`server/`.

## Status

The migration to a server-owned implementation is complete:

- `noisebot_server` is the importable package under `server/`.
- `noisebot-server` owns the CLI, normal runtime shell, debug tools and service
  management.
- `NoiseBotServer` owns application composition and lifecycle end to end.
- Every internal boundary (`transport`, `ops`, `agent`, `vision`) is implemented
  and owned by `noisebot_server` — none of them delegate to `bridge_v2` at
  runtime.

## Transport boundary

`noisebot_server.internal.transport` owns the firmware connection: TCP/UART
transport, framing/protocol codec and reconnect supervision.

```python
from noisebot_server.internal.transport import TcpTransport
from noisebot_server.internal.transport.protocol import encode_frame
```

## Ops boundary

`noisebot_server.internal.ops` is the server-owned surface for local
operations: health, status, metrics, debug, config, device persona cache and
token checks, serving the existing dashboard/API.

```python
from noisebot_server.internal.ops import OpsHttpServer, StatusStore
```

## Agent boundary

`noisebot_server.internal.agent` owns the voice/agent pipeline: conversation
runtime, local intents, STT, LLM providers, TTS and orchestration — including
voice, barge-in, local LLM configuration and Piper playback behavior.

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

Endpoints marked as implemented are served by `noisebot_server` itself.
Reserved endpoints describe the next API surface for app, vision, agent, device
and agenda work.

## Runtime shell

Normal `noisebot-server` startup is now owned by `noisebot_server.cli` and
`noisebot_server.runtime`. The server package owns config loading, logging,
event-loop startup and the main application graph.

## Application graph

`NoiseBotServer` composes the runtime inside `noisebot_server.app` as a
self-contained application graph. Transport, ops, agent and service
dependencies are all imported through `noisebot_server.internal.*` boundaries,
owned end to end by the server package.

## Debug tools

The `debug` subcommands are owned by `noisebot_server`:

```powershell
python -m noisebot_server debug transcript "que horas sao"
python -m noisebot_server debug fake-fw --port 9001
```

`debug transcript` injects a synthetic transcript into the server agent path.
`debug fake-fw` starts a TCP firmware simulator that speaks the same wire
protocol used by the ESP32.

## Service tools

The `service` subcommands are owned by `noisebot_server`:

```powershell
python -m noisebot_server service status
python -m noisebot_server service install
python -m noisebot_server service start
python -m noisebot_server service stop
```

On Windows this registers the `NoiseBot Server` Task Scheduler entry. On Linux
it writes a user-level `noisebot-server.service` unit.
