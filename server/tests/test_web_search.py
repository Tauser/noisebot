"""Testes da tool web_search (Tavily-only, server-side).

Sem internet real — urlopen e sempre mockado.
Cobre: sanitizacao, limites, modos, provider, cache, dedupe, validacao de URL,
normalizacao, erros do Tavily, formatacao para LLM, e nao-vazamento da API key,
alem da integracao com catalog/gateway.
"""
from __future__ import annotations

import io
import json
from urllib.error import HTTPError

import pytest

from noisebot_server.internal.agent import web_search as ws
from noisebot_server.internal.agent.tools.catalog import CATALOG, validate_arguments
from noisebot_server.internal.agent.tools.gateway import execute_tool_call
from noisebot_server.internal.agent.orchestrator import _allowed_tools


@pytest.fixture(autouse=True)
def _clear_cache():
    ws._CACHE.clear()
    yield
    ws._CACHE.clear()


class _Resp(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()
        return False


def _mock_tavily(monkeypatch, payload: dict, counter: list | None = None):
    data = json.dumps(payload).encode("utf-8")

    def _fake(request, timeout=None):
        assert "tavily.com" in request.full_url
        if counter is not None:
            counter.append(1)
        return _Resp(data)

    monkeypatch.setattr(ws, "urlopen", _fake)


_OK_PAYLOAD = {
    "answer": "A Copa comeca em 11 de junho de 2026.",
    "results": [
        {"title": "Calendario Copa 2026", "content": "Brasil estreia em 14/06.",
         "url": "https://example.com/copa", "published_date": "2026-06-09", "score": 0.91},
    ],
}


# === 1-3: sanitizacao =======================================================

def test_01_query_vazia(monkeypatch):
    monkeypatch.setenv("TAVILY_API_KEY", "k")
    assert ws.fetch_web_search("   ").error == "consulta vazia"


def test_02_espacos_excessivos():
    assert ws._sanitize_query("  jogos   do    brasil  ") == "jogos do brasil"


def test_03_remove_controle():
    out = ws._sanitize_query("a\x00b\x07c\x1fd")
    assert "\x00" not in out and "\x07" not in out
    assert out == "a b c d"


# === 4-7: limites ===========================================================

def test_04_max_results_min():
    assert ws._cap_max_results(0) == 1


def test_05_max_results_max():
    assert ws._cap_max_results(99) == 8


def test_06_timeout_min():
    assert ws._cap_timeout(0.01) == 1.0


def test_07_timeout_max():
    assert ws._cap_timeout(999) == 15.0


# === 8-9: mode / provider ===================================================

def test_08_mode_invalido_cai_para_auto(monkeypatch):
    monkeypatch.setenv("TAVILY_API_KEY", "k")
    assert ws._normalize_mode("banana") == "auto"
    _mock_tavily(monkeypatch, _OK_PAYLOAD)
    resp = ws.fetch_web_search("teste", mode="banana")
    assert resp.mode == "auto"


def test_09_provider_invalido(monkeypatch):
    monkeypatch.setenv("TAVILY_API_KEY", "k")
    resp = ws.fetch_web_search("teste", provider="brave")
    assert resp.error == "provider inválido"


# === 10: sem API key ========================================================

def test_10_tavily_sem_key(monkeypatch):
    monkeypatch.delenv("TAVILY_API_KEY", raising=False)
    resp = ws.fetch_web_search("teste")
    assert resp.results == []
    assert "TAVILY_API_KEY" in resp.error


# === 11-13: cache ===========================================================

def test_11_cache_miss(monkeypatch):
    monkeypatch.setenv("TAVILY_API_KEY", "k")
    calls: list = []
    _mock_tavily(monkeypatch, _OK_PAYLOAD, calls)
    resp = ws.fetch_web_search("copa", mode="general")
    assert resp.cached is False
    assert len(calls) == 1


def test_12_cache_hit(monkeypatch):
    monkeypatch.setenv("TAVILY_API_KEY", "k")
    calls: list = []
    _mock_tavily(monkeypatch, _OK_PAYLOAD, calls)
    ws.fetch_web_search("copa", mode="general")
    resp2 = ws.fetch_web_search("copa", mode="general")
    assert resp2.cached is True
    assert len(calls) == 1  # segunda chamada nao bateu na rede


def test_13_cache_desabilitado(monkeypatch):
    monkeypatch.setenv("TAVILY_API_KEY", "k")
    calls: list = []
    _mock_tavily(monkeypatch, _OK_PAYLOAD, calls)
    ws.fetch_web_search("copa", use_cache=False)
    ws.fetch_web_search("copa", use_cache=False)
    assert len(calls) == 2


# === 14-15: dedupe ==========================================================

def test_14_dedupe_por_url():
    hits = [
        ws.SearchHit("A", "s", "https://x.com/p"),
        ws.SearchHit("B", "s", "https://www.x.com/p/"),  # mesma URL normalizada
        ws.SearchHit("C", "s", "https://y.com/p"),
    ]
    out = ws._dedupe_hits(hits)
    assert len(out) == 2
    assert [h.title for h in out] == ["A", "C"]


def test_15_dedupe_por_titulo():
    hits = [
        ws.SearchHit("Mesmo Titulo", "s", ""),
        ws.SearchHit("mesmo titulo", "s", ""),
    ]
    assert len(ws._dedupe_hits(hits)) == 1


# === 16-19: validacao de URL ================================================

def test_16_bloqueia_javascript():
    assert ws._is_safe_result_url("javascript:alert(1)") is False


def test_17_bloqueia_file():
    assert ws._is_safe_result_url("file:///etc/passwd") is False


def test_18_bloqueia_data():
    assert ws._is_safe_result_url("data:text/html,<b>x</b>") is False


def test_19_aceita_https():
    assert ws._is_safe_result_url("https://example.com") is True


# === 20-21: normalizacao ====================================================

def test_20_strip_html_entidades():
    out = ws._strip_html("<b>Brasil</b> &amp; Argentina")
    assert "<" not in out and "&amp;" not in out
    assert "Brasil" in out and "& Argentina" in out


def test_21_extract_hostname():
    assert ws._extract_hostname("https://www.lance.com.br/tabela") == "lance.com.br"
    assert ws._extract_hostname("not a url") in ("", "unknown")


# === 22-24: payloads Tavily =================================================

def test_22_payload_valido(monkeypatch):
    monkeypatch.setenv("TAVILY_API_KEY", "k")
    _mock_tavily(monkeypatch, _OK_PAYLOAD)
    resp = ws.fetch_web_search("copa", max_results=3)
    assert resp.source == "Tavily"
    assert resp.error is None
    assert resp.results[0].title == "Calendario Copa 2026"
    assert resp.results[0].source == "example.com"
    assert resp.results[0].published == "2026-06-09"
    assert resp.results[0].score == 0.91


def test_23_payload_sem_results(monkeypatch):
    monkeypatch.setenv("TAVILY_API_KEY", "k")
    _mock_tavily(monkeypatch, {"answer": "x"})
    resp = ws.fetch_web_search("copa")
    assert resp.error == "resposta inválida de Tavily"


def test_24_payload_invalido(monkeypatch):
    monkeypatch.setenv("TAVILY_API_KEY", "k")

    def _fake(request, timeout=None):
        return _Resp(b"[1, 2, 3]")  # JSON valido mas nao e dict
    monkeypatch.setattr(ws, "urlopen", _fake)
    resp = ws.fetch_web_search("copa")
    assert resp.error == "resposta inválida de Tavily"


# === 25-27: erros HTTP / timeout ============================================

def test_25_http_401(monkeypatch):
    monkeypatch.setenv("TAVILY_API_KEY", "k")

    def _raise(request, timeout=None):
        raise HTTPError(request.full_url, 401, "Unauthorized", {}, io.BytesIO(b""))
    monkeypatch.setattr(ws, "urlopen", _raise)
    resp = ws.fetch_web_search("copa")
    assert resp.error == "erro HTTP 401 em Tavily"


def test_26_http_429(monkeypatch):
    monkeypatch.setenv("TAVILY_API_KEY", "k")

    def _raise(request, timeout=None):
        raise HTTPError(request.full_url, 429, "Too Many", {}, io.BytesIO(b""))
    monkeypatch.setattr(ws, "urlopen", _raise)
    resp = ws.fetch_web_search("copa")
    assert resp.error == "erro HTTP 429 em Tavily"


def test_27_timeout(monkeypatch):
    monkeypatch.setenv("TAVILY_API_KEY", "k")

    def _raise(request, timeout=None):
        raise TimeoutError("timed out")
    monkeypatch.setattr(ws, "urlopen", _raise)
    resp = ws.fetch_web_search("copa")
    assert resp.error == "timeout ao consultar Tavily"


# === 28-31: formatacao para LLM =============================================

def test_28_format_com_resultados():
    resp = ws.SearchResponse(
        query="copa", source="Tavily", mode="news",
        answer="Resumo X.",
        results=[ws.SearchHit("Titulo A", "trecho", "https://a.com",
                              published="2026-06-09", source="a.com", score=0.8)],
    )
    text = ws.format_search_results_for_llm(resp)
    assert 'Resultados da busca por: "copa"' in text
    assert "Provider: Tavily" in text
    assert "Modo: news" in text
    assert "dados externos nao confiaveis" in text
    assert "[S1]" in text and "Titulo A" in text and "https://a.com" in text
    assert "Resumo X." in text


def test_29_format_com_erro():
    text = ws.format_search_results_for_llm(
        ws.SearchResponse(query="x", source="Tavily", error="erro HTTP 401 em Tavily")
    )
    assert "falhou" in text and "401" in text


def test_30_format_sem_resultados():
    text = ws.format_search_results_for_llm(ws.SearchResponse(query="x", source="Tavily"))
    assert "Nenhum resultado" in text


def test_31_format_limita_max_chars():
    hits = [ws.SearchHit(f"Titulo {i}", "x" * 280, f"https://s{i}.com") for i in range(8)]
    resp = ws.SearchResponse(query="q", source="Tavily", results=hits)
    text = ws.format_search_results_for_llm(resp, max_chars=400)
    assert len(text) <= 400


# === 32: cached marca True ==================================================

def test_32_cached_true(monkeypatch):
    monkeypatch.setenv("TAVILY_API_KEY", "k")
    _mock_tavily(monkeypatch, _OK_PAYLOAD)
    ws.fetch_web_search("copa")
    resp = ws.fetch_web_search("copa")
    assert resp.cached is True
    assert "Cache: sim" in ws.format_search_results_for_llm(resp)


# === 33: nao vazar API key ==================================================

def test_33_nao_vaza_api_key(monkeypatch):
    monkeypatch.setenv("TAVILY_API_KEY", "super-secret-123")

    def _raise(request, timeout=None):
        raise HTTPError(request.full_url, 401, "Unauthorized", {}, io.BytesIO(b""))
    monkeypatch.setattr(ws, "urlopen", _raise)
    resp = ws.fetch_web_search("copa")
    assert "super-secret-123" not in (resp.error or "")
    assert "super-secret-123" not in ws.format_search_results_for_llm(resp)


# === build_provider_query (bonus de cobertura) ==============================

def test_build_provider_query_modos():
    assert ws._build_provider_query("esp-idf", "technical") != "esp-idf"
    assert "documenta" in ws._build_provider_query("esp-idf", "technical").lower()
    assert ws._build_provider_query("dolar hoje", "factual") == "dolar hoje"
    assert "noticias" in ws._build_provider_query("eleicao", "news").lower()


# === Integracao: catalog / allowed_tools / gateway ==========================

def test_catalog_expoe_mode():
    spec = CATALOG["web_search"]
    props = spec.arguments_schema["properties"]
    assert "mode" in props
    assert set(props["mode"]["enum"]) == {"auto", "general", "factual", "news", "technical"}
    assert validate_arguments(spec, {"query": "oi", "mode": "news"}) == []
    assert validate_arguments(spec, {"query": "oi", "mode": "x"}) != []  # enum invalido


def test_web_search_em_allowed_tools():
    assert "web_search" in _allowed_tools(vision_available=False)


def test_gateway_executa_web_search(monkeypatch):
    fake = ws.SearchResponse(
        query="capital franca", source="Tavily", provider="tavily", mode="factual",
        answer="Paris.",
        results=[ws.SearchHit("Paris", "Paris e a capital.", "https://x.com", source="x.com")],
    )
    monkeypatch.setattr(
        "noisebot_server.internal.agent.web_search.fetch_web_search",
        lambda *a, **k: fake,
    )
    result = execute_tool_call(
        {"name": "web_search", "arguments": {"query": "capital franca", "mode": "factual"}},
        current_state="IDLE", turn_id=1,
    )
    assert result.success
    assert result.result["source"] == "Tavily"
    assert result.result["mode"] == "factual"
    assert "Paris" in result.result["summary"]


def test_gateway_veta_sem_query():
    result = execute_tool_call(
        {"name": "web_search", "arguments": {}}, current_state="IDLE", turn_id=2,
    )
    assert result.vetoed


# === sources determinísticas (orchestrator._extract_sources) ================

def test_extract_sources_da_web_search():
    from types import SimpleNamespace
    from noisebot_server.internal.agent.orchestrator import _extract_sources

    tr = SimpleNamespace(success=True, result={
        "results": [
            {"title": "A", "source": "a.com", "url": "https://a.com/1"},
            {"title": "B", "source": "b.com", "url": "https://a.com/1"},  # url duplicada
            {"title": "C", "source": "c.com", "url": "https://c.com/2"},
            {"title": "D", "source": "", "url": ""},  # sem url -> ignorado
        ]
    })
    out = _extract_sources(tr)
    assert out == [
        {"title": "A", "source": "a.com", "url": "https://a.com/1"},
        {"title": "C", "source": "c.com", "url": "https://c.com/2"},
    ]


def test_extract_sources_vazio_quando_sem_tool():
    from noisebot_server.internal.agent.orchestrator import _extract_sources
    from types import SimpleNamespace
    assert _extract_sources(None) == []
    assert _extract_sources(SimpleNamespace(success=False, result={})) == []
    assert _extract_sources(SimpleNamespace(success=True, result={"results": "x"})) == []


def test_extract_sources_limita(monkeypatch):
    from types import SimpleNamespace
    from noisebot_server.internal.agent.orchestrator import _extract_sources
    tr = SimpleNamespace(success=True, result={
        "results": [{"title": str(i), "source": "s", "url": f"https://s.com/{i}"} for i in range(10)]
    })
    assert len(_extract_sources(tr, limit=5)) == 5


# === entrega das sources no bus (RobotOutputProvider) =======================

def test_sources_publicadas_no_bus():
    import asyncio
    from noisebot_server.internal.agent.output import RobotOutputProvider
    from noisebot_server.internal.agent.runtime import IntentResolved, RobotCommand

    class _FakeBus:
        def __init__(self):
            self.published = []

        async def publish(self, ev):
            self.published.append(ev)

    class _FakeAdapter:
        def __init__(self):
            self.calls = []

        async def send_text_scroll(self, text):
            self.calls.append(("text", text))
        # propositalmente SEM send_sources: 'sources' nunca deve ir ao firmware.

    async def _run():
        bus = _FakeBus()
        adapter = _FakeAdapter()
        rop = RobotOutputProvider(bus)
        intent = IntentResolved(
            turn_id=7, intent_name="llm_reply", reply_text="oi",
            sources=[{"title": "A", "source": "a.com", "url": "https://a.com/1"}],
        )
        await rop.emit_for_intent(intent, adapter)
        return bus.published, adapter.calls

    published, calls = asyncio.run(_run())
    src_cmds = [c for c in published if isinstance(c, RobotCommand) and c.kind == "sources"]
    assert len(src_cmds) == 1
    assert src_cmds[0].payload["sources"][0]["url"] == "https://a.com/1"
    # firmware recebeu so o texto, nunca 'sources'
    assert ("text", "oi") in calls
    assert all(kind != "sources" for kind, _ in calls)


def test_sources_vazias_nao_emitem_comando():
    import asyncio
    from noisebot_server.internal.agent.output import RobotOutputProvider
    from noisebot_server.internal.agent.runtime import IntentResolved, RobotCommand

    class _FakeBus:
        def __init__(self):
            self.published = []

        async def publish(self, ev):
            self.published.append(ev)

    async def _run():
        bus = _FakeBus()
        rop = RobotOutputProvider(bus)
        intent = IntentResolved(turn_id=8, intent_name="llm_reply", reply_text="oi")
        await rop.emit_for_intent(intent, adapter=None)
        return bus.published

    published = _run.__wrapped__() if hasattr(_run, "__wrapped__") else asyncio.run(_run())
    assert all(not (isinstance(c, RobotCommand) and c.kind == "sources") for c in published)
