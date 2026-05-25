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
