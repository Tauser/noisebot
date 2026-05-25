# Internal

Server implementation code. Keep product logic here instead of inside firmware.

Layout:

- `boot/`: startup and lifecycle.
- `controller/`: HTTP/API handlers.
- `service/`: business services.
- `model/`: data models.
- `transport/`: firmware TCP/UART adapters.
- `ops/`: local diagnostics aggregation.
- `vision/`: host-side vision services.
- `agent/`: STT/LLM/TTS orchestration.
