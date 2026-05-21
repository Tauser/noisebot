"""bridgev2.ops.http_api — Servidor HTTP local-only async (Fase 9.5).

Faz bind em localhost por padrão. Não bloqueia o loop principal.
Uma falha da API de operação NÃO derruba o pipeline de voz.

Endpoints planejados (§11 do BRIDGE_V2.md):
  GET  /health
  GET  /ai/status
  GET  /ai/metrics
  GET  /ai/errors
  GET  /ai/config
  POST /ai/config
  POST /ai/mode
  POST /ai/restart
  POST /ai/metrics/reset
"""
from __future__ import annotations
# TODO Fase 9.5: OpsHttpServer com aiohttp
