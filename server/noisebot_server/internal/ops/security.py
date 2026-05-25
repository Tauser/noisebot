"""noisebot_server.internal.ops.security — token local + allowlist de IP.

Regras:
- Token obrigatório em todo POST (Authorization: Bearer <token> ou ?token=<token>).
- GET endpoints: token opcional (recomendado mas não obrigatório por padrão).
- Bind em localhost por padrão — só aceita IPs da allowlist para acesso externo.
- A API key NUNCA aparece em nenhum lugar deste módulo.
"""
from __future__ import annotations

import logging
import os
import secrets
from pathlib import Path
from typing import Iterable

from aiohttp import web

log = logging.getLogger(__name__)

_TOKEN_FILE = Path.home() / ".noisebot-server" / "ops_token"
_LOCALHOST = {"127.0.0.1", "::1", "localhost"}


def load_or_create_token() -> str:
    """Carrega token de NOISEBOT_OPS_TOKEN, arquivo local ou cria um novo."""
    env_token = os.environ.get("NOISEBOT_OPS_TOKEN", "").strip()
    if env_token:
        return env_token

    if _TOKEN_FILE.exists():
        token = _TOKEN_FILE.read_text(encoding="utf-8").strip()
        if token:
            return token

    token = secrets.token_hex(32)
    _TOKEN_FILE.parent.mkdir(parents=True, exist_ok=True)
    _TOKEN_FILE.write_text(token, encoding="utf-8")
    log.info(
        "Ops API: token gerado e salvo em %s "
        "(ou configure NOISEBOT_OPS_TOKEN no ambiente)",
        _TOKEN_FILE,
    )
    return token


def _extract_token(request: web.Request) -> str:
    """Extrai token do header Authorization ou query param ?token=."""
    auth = request.headers.get("Authorization", "")
    if auth.lower().startswith("bearer "):
        return auth[7:].strip()
    return request.rel_url.query.get("token", "").strip()


def check_token(request: web.Request, expected: str) -> bool:
    """Verifica token da requisição (timing-safe)."""
    provided = _extract_token(request)
    if not provided or not expected:
        return False
    return secrets.compare_digest(provided, expected)


def check_ip(request: web.Request, allowed_ips: Iterable[str] | None) -> bool:
    """Verifica se o IP do cliente está na allowlist.

    Se allowed_ips é None, aceita qualquer IP (modo padrão localhost-only).
    O bind em localhost já impede IPs externos por padrão.
    """
    if allowed_ips is None:
        return True
    client_ip = request.remote or ""
    return client_ip in set(allowed_ips) | _LOCALHOST


def require_token(expected_token: str):
    """Decorator que exige token válido para handlers POST."""
    def decorator(handler):
        async def wrapper(request: web.Request):
            if not check_token(request, expected_token):
                raise web.HTTPUnauthorized(
                    text='{"error": "token inválido ou ausente"}',
                    content_type="application/json",
                )
            return await handler(request)
        return wrapper
    return decorator
