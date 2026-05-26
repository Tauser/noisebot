# NoiseBot

NoiseBot is an offline-first expressive desktop companion robot built on ESP32-S3,
with a local bridge/app layer for heavier product features.

The repository now follows the same product split idea used by StackChan, but
only with the parts NoiseBot needs now:

```text
Noisebot/
├── firmware/   # ESP-IDF firmware boundary; current firmware still lives at repo root
├── server/     # Local companion server: bridge, ops API, dashboard backend, AI adapters
├── app/        # External dashboard/client
├── components/ # Current ESP-IDF components
├── main/       # Current ESP-IDF app_main
├── bridge_v2/  # Current Python bridge implementation
└── docs/       # Architecture, roadmap, hardware and integration docs
```

`server/` keeps a StackChan-like separation (`api/`, `internal/`, `manifest/`,
`resource/`) without copying modules NoiseBot does not have.

The firmware remains lean: no embedded dashboard, no firmware WebSocket UI, and
no generic SD file manager. Rich diagnostics and operator UI belong in
`server/` and `app/`.

## Desenvolvimento local

Para subir o server local e o dashboard em modo desenvolvimento:

```powershell
.\dev.ps1
```

Por padrao, o script usa o robo em `192.168.1.30:9000`, carrega
`server/.env`, abre o NoiseBot Server em `http://127.0.0.1:8765` e o
dashboard em `http://127.0.0.1:5173`.

Exemplo com outro IP:

```powershell
.\dev.ps1 -RobotHost 192.168.1.50
```

Quando o build do app existe em `app/dist`, o `noisebot_server` tambem serve o
dashboard diretamente em `http://127.0.0.1:8765`. O Vite em `5173` continua
existindo apenas para desenvolvimento com hot reload.
