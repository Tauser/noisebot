"""Helper único para HTTP de saída (GET) bloqueante e assíncrono.

Motivação (SF-07, docs/ANALISE_SERVER_FINDINGS_2026-06-11.md): vários módulos
chamam `urllib.request.urlopen` diretamente; cada call site precisa lembrar de
envolver a chamada em `asyncio.to_thread` quando usado a partir do event loop
(SF-01 mostrou que isso é facil de esquecer). Este modulo centraliza a chamada
HTTP e oferece uma versao async pronta.

Uso:
    from ..http_fetch import fetch_json, fetch_json_async

    payload = fetch_json(url, timeout_s=3.0)               # caminho sync
    payload = await fetch_json_async(url, timeout_s=3.0)   # caminho async (event loop)
"""

from __future__ import annotations

import asyncio
import json
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

DEFAULT_USER_AGENT = "NoiseBot-Server/0.1"


class HttpFetchError(Exception):
    """Erro de rede, timeout ou JSON invalido ao buscar uma URL."""


def fetch_json(
    url: str,
    *,
    timeout_s: float = 3.0,
    headers: dict[str, str] | None = None,
) -> dict:
    """Busca `url` e decodifica a resposta como JSON. Bloqueante.

    Levanta HttpFetchError em caso de falha de rede, timeout ou JSON invalido.
    Chamadores no event loop devem usar `fetch_json_async`.
    """
    req_headers = {"User-Agent": DEFAULT_USER_AGENT}
    if headers:
        req_headers.update(headers)
    request = Request(url, headers=req_headers)
    try:
        with urlopen(request, timeout=timeout_s) as response:
            return json.loads(response.read().decode("utf-8"))
    except (HTTPError, URLError, TimeoutError, OSError, json.JSONDecodeError) as exc:
        raise HttpFetchError(str(exc)) from exc


async def fetch_json_async(
    url: str,
    *,
    timeout_s: float = 3.0,
    headers: dict[str, str] | None = None,
) -> dict:
    """Versao async de `fetch_json` — roda fora do event loop via `asyncio.to_thread`."""
    return await asyncio.to_thread(fetch_json, url, timeout_s=timeout_s, headers=headers)
