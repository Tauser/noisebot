"""bridgev2.ops.security — Token local + bind localhost + allowlist de IP (Fase 9.5).

Regra: token local E bind localhost por padrão.
Mesmo em LAN, endpoints de configuração exigem proteção.
API key NUNCA aparece em nenhuma resposta ou log.
"""
from __future__ import annotations
# TODO Fase 9.5: middleware de autenticação para aiohttp
