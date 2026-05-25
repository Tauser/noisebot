# NoiseBot App

External dashboard/client for NoiseBot.

This mirrors StackChan's `app/` boundary. It is the correct place for rich UI,
camera preview, diagnostics, setup flows and future mobile/desktop controls.

The app should talk to the local server first, and only use firmware REST
endpoints through explicit server adapters when needed. The app must not depend
on ESP32 routes directly.

Current phase:

- No framework is selected yet.
- No Android/iOS folders are created yet.
- The first contract is server-first and lives in
  `server/noisebot_server/api/contract.py`.
- A future UI should consume only implemented server endpoints until reserved
  endpoints become real server routes.

Suggested layout:

```text
app/
├── lib/     # Client application source
├── assets/  # App-owned assets
└── test/    # Client tests
```
