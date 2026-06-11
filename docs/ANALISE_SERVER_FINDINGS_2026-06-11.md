# NoiseBot — Revisão Estruturada do `/server` (Python)

**Data:** 2026-06-11 · **Escopo:** `server/noisebot_server/` (~36k linhas com testes), `server/tests/`, `pyproject.toml`, layout do diretório.
**Método:** leitura de app.py, config.py, security.py, gateway.py, intents.py, orchestrator (dirigida), llm/circuit_breaker, weather/web_search, transport/adapter, vision/, service/manager, ops/http; greps de padrões de risco (blocking-in-async, subprocess, secrets, eval).
**Mesmos campos do relatório de firmware:** Evidência · Severidade P0–P3 · Categoria · Forma correta · Menor correção segura · Validação.

---

## 0. Autoridade (CLAUDE.md) aplicada ao server — veredito

O CLAUDE.md e o AGENTS.md (cópias quase idênticas, **ambos** declarando "autoridade máxima") são 100% firmware-cêntricos: nenhuma regra cobre o server Python. Consequências práticas:

1. **Não existe autoridade para o server** — convenções reais do código (offline-first, secrets só por env, token obrigatório em POST, bind localhost) não estão escritas em lugar nenhum; uma sessão de IA trabalhando no server não tem regras para seguir. → finding SF-12.
2. **Dois arquivos disputam "autoridade máxima"** (CLAUDE.md e AGENTS.md). Se divergirem um dia, não há desempate definido. → consolidar: um arquivo canônico e o outro como ponteiro.
3. As regras de firmware aplicáveis por analogia (offline-first; "conectividade é conveniência") **são cumpridas** pelo server — intents locais respondem sem LLM, circuit breaker degrada com provider fora.

---

## 1. Achados

### SF-01 — `LocalIntentProvider` declara "sem I/O" mas faz rede bloqueante no event loop
- **Evidência:** `internal/agent/intents.py:485–489` (docstring: "determinístico, sem I/O"); `:831` (`fetch_weather_now()` → `urlopen` bloqueante com timeout 3 s, `weather.py:61,78`); `:845–863` (`_match_vision` → `self._vision.observe()/analyze()` síncronos, presumivelmente HTTP para câmera/Ollama). Chamado direto do caminho async: `orchestrator.py:545` (comentário "Resolve intent local (< 5 ms, sem I/O)") e `:559`.
- **Severidade:** P1 · **Categoria:** erro real (o contrato declarado é violado pelo próprio módulo).
- **Forma correta:** intents puros retornam um *intent diferido* (nome + parâmetros); o orchestrator resolve I/O via `asyncio.to_thread` — o mesmo padrão que o projeto já usa corretamente em `web_search.py:472–475` e `orchestrator.py:973,1215`.
- **Menor correção segura:** no orchestrator, envolver a chamada `self._intent.match(...)` em `await asyncio.to_thread(...)` (1 linha; match não toca estado compartilhado mutável).
- **Validação:** teste com mock de weather lento (3 s): heartbeat/keep-alive do bridge e streaming de áudio não param durante a resolução; atualizar a docstring.

### SF-02 — Link bridge firmware↔server sem autenticação (achado cross-sistema)
- **Evidência:** firmware aceita qualquer cliente TCP na porta 9000 (`components/infra/bridge_service.h:17–20`); handshake HELLO sem token (`server/internal/transport/adapter.py:165` e grep de auth/token vazio no adapter). Qualquer host da LAN pode conectar no ESP32 e enviar frames SAY/EXPR/ACTION/VOLUME válidos (CRC8 é integridade, não autenticação).
- **Severidade:** P1 · **Categoria:** erro real (segurança; espelho do F12 do firmware).
- **Forma correta:** segredo compartilhado provisionado (NVS no firmware, `~/.noisebot-server/` no server) validado no HELLO; firmware derruba conexão sem token correto; rate-limit de tentativas.
- **Menor correção segura:** campo token no payload do HELLO (o framing já comporta DATA variável) com comparação no firmware; mudança pequena nos dois lados, retrocompatível por flag.
- **Validação:** teste de contrato novo em `tests/test_firmware_bridge_contract.py`: HELLO sem/com token errado → conexão recusada.

### SF-03 — Testes exigem Python 3.11, metadado declara ≥3.10
- **Evidência:** `pyproject.toml:9` (`requires-python = ">=3.10"`); `tests/test_llm_integration.py:106,422` (`asyncio.timeout` — só existe em 3.11+). Confirmado em ambiente 3.10: a suite quebra (falha ambiental conhecida, registrada em memória do projeto).
- **Severidade:** P2 · **Categoria:** erro real (metadado mente sobre o suporte).
- **Forma correta:** decidir o piso: ou `requires-python = ">=3.11"`, ou compatibilidade real com 3.10 trocando `asyncio.timeout` por `asyncio.wait_for`.
- **Menor correção segura:** trocar as 2 ocorrências por `asyncio.wait_for` (são em testes; semântica equivalente nesses usos).
- **Validação:** suite verde em 3.10 **e** 3.11 (matriz de CI, ver SF-10).

### SF-04 — orchestrator.py: god module em formação
- **Evidência:** `internal/agent/orchestrator.py` — 2.014 linhas acumulando Fases 3–15 (docstring `:12–21`): FSM de turno, STT, intents, LLM streaming, tools two-step, playback, persona sync, diag. Tendência idêntica ao web_service.c do firmware (F09).
- **Severidade:** P2 · **Categoria:** funciona mas errado (ainda coeso, mas no limite).
- **Forma correta:** extrair colaboradores por fase: `turn_pipeline.py` (STT→intent→LLM), `tool_loop.py` (two-step), `persona_sync.py`, mantendo o orchestrator como coordenador fino.
- **Menor correção segura:** extrair o bloco de persona/firmware-snapshot (`:1215–1260`) — já isolado por `to_thread` — para módulo próprio.
- **Validação:** `tests/test_server_facade.py` verde sem alteração (é teste de fachada — protege exatamente esse refactor).

### SF-05 — test_server_facade.py monolítico
- **Evidência:** `tests/test_server_facade.py` — 10.122 linhas num único arquivo (28% de todo o código do server).
- **Severidade:** P3 · **Categoria:** funciona mas errado (tempo de coleta, conflitos de merge, navegação).
- **Forma correta:** dividir por domínio (test_facade_turns, test_facade_tools, test_facade_playback, test_facade_vision, test_facade_ops) mantendo fixtures num conftest.
- **Menor correção segura:** split mecânico por classes de teste, sem tocar nos corpos.
- **Validação:** `pytest --collect-only -q` conta o mesmo número de testes antes/depois.

### SF-06 — Scaffold fantasma duplicando o layout do pacote
- **Evidência:** `server/api/`, `server/internal/`, `server/manifest/`, `server/resource/` na raiz contêm apenas `.gitkeep` e READMEs, espelhando os nomes reais de `noisebot_server/api` e `noisebot_server/internal`. O ROADMAP referencia caminho inexistente: "server/noisebot_server/internal/vision/face_service.py" (ROADMAP.md, Feito Consolidado) — o diretório real tem `analysis.py`, `client.py`, `face_loop.py`.
- **Severidade:** P3 · **Categoria:** erro real documental (induz humano e IA a editar a árvore errada).
- **Forma correta:** apagar o scaffold vazio (ou, se a intenção era migrar para layout raiz, registrar a decisão e migrar de uma vez); corrigir a referência do ROADMAP.
- **Menor correção segura:** `git rm` dos diretórios com só `.gitkeep` — zero impacto em imports (verificado: nenhum .py).
- **Validação:** suite de testes verde; grep por referências aos caminhos removidos.

### SF-07 — `urlopen` síncrono espalhado em módulos servidos pelo aiohttp
- **Evidência:** padrão repetido em `internal/ops/aec_live.py:9,147`, `internal/debug/manual.py:11,136`, `internal/agent/weather.py:8,78` — só `web_search.py` tem o wrapper async correto (`:472–475`).
- **Severidade:** P2 · **Categoria:** funciona mas errado (cada call site precisa lembrar do `to_thread`; SF-01 prova que esquecem).
- **Forma correta:** helper único `internal/http_fetch.py` com versão sync e async (`to_thread` embutido); regra de lint proibindo `urlopen` fora dele.
- **Menor correção segura:** criar o helper e migrar apenas `weather.py` (o único comprovadamente no caminho quente).
- **Validação:** grep de CI: `urlopen` aparece só no helper; teste do SF-01 cobre o caminho quente.

### SF-08 — Logs de runtime na raiz do diretório do server
- **Evidência:** `server/runtime.log`, `server/noisebot_server_restart.out.log`, `server/noisebot_server_restart.err.log`, `server/logs/` (não versionados — `git ls-files` limpo — mas vivem misturados ao código).
- **Severidade:** P3 · **Categoria:** funciona mas errado.
- **Forma correta:** logs em `~/.noisebot-server/logs/` (o diretório já existe para o ops_token) com rotação; caminho configurável por env.
- **Menor correção segura:** mudar o caminho default de escrita e adicionar os padrões ao .gitignore.
- **Validação:** server reiniciado escreve no novo caminho; raiz limpa.

### SF-09 — Comando PowerShell montado por f-string no service manager
- **Evidência:** `internal/service/manager.py:90–99` — `Get-ScheduledTask -TaskName '{TASK_NAME}'` interpolado em string passada ao PowerShell. Hoje `TASK_NAME` é constante interna (risco real baixo), mas o padrão é frágil se algum dia virar configurável.
- **Severidade:** P3 · **Categoria:** funciona mas errado (padrão, não vulnerabilidade atual).
- **Forma correta:** passar argumentos fora da string (parâmetros do PowerShell) ou validar com allowlist `[A-Za-z0-9_-]+` na borda.
- **Menor correção segura:** assert de formato sobre `TASK_NAME` no topo do módulo.
- **Validação:** testes do manager nos dois caminhos (nome válido/ inválido).

### SF-10 — CI inexistente para o server (e para tudo)
- **Evidência:** nenhum workflow no repo (sem `.github/`, sem pipeline); a suite (18 arquivos, incluindo testes de contrato firmware↔server valiosos) só roda manualmente; SF-03 passou despercebido exatamente por isso.
- **Severidade:** P2 · **Categoria:** inexistente.
- **Forma correta:** CI com matriz Python 3.10/3.11 (ou só 3.11 após SF-03), lint (ruff) e a suite; gancho futuro para o build host do firmware (F43).
- **Menor correção segura:** workflow mínimo: `pip install -e .[dev] && pytest` em 3.11.
- **Validação:** pipeline verde no estado atual; PR com warning novo falha.

### SF-11 — Dashboard ops: GETs sem token por design — aceitável, documentar o limite
- **Evidência:** `internal/ops/security.py:1–8` (regras explícitas: POST exige token, GET opcional, bind 127.0.0.1, allowlist p/ externo); `internal/ops/http.py:218–221` (`_require_token` nos POSTs); `config.py:123` ("without secret values" — expõe só `*_configured` bools, `:311–312,348`).
- **Severidade:** P3 · **Categoria:** acerto com ressalva.
- **Ressalva:** se a allowlist liberar IPs externos, todos os GETs de status/config ficam legíveis na LAN. Sem secrets expostos (verificado), mas telemetria do ambiente vaza.
- **Menor melhoria:** flag `NOISEBOT_OPS_GET_TOKEN=1` para ambientes com allowlist aberta.
- **Validação:** GET com flag ativa e sem token → 401.

### SF-12 — Server sem documento de autoridade (e duas "autoridades máximas" no repo)
- **Evidência:** CLAUDE.md e AGENTS.md (raiz) quase idênticos, ambos "autoridade máxima", ambos 100% firmware; zero regras escritas para `server/` — as convenções reais (secrets só por env — `llm.py:792,820`; token em POST; bind localhost; offline-first com intents locais) existem só no código.
- **Severidade:** P2 (multiplicador para sessões de IA) · **Categoria:** inexistente.
- **Forma correta:** seção "Server" no CLAUDE.md (ou `server/CLAUDE.md`) com: layout do pacote (apontando que `noisebot_server/` é o real — ver SF-06), regras de async (nunca I/O bloqueante no loop — SF-01/SF-07), política de secrets/token, piso de Python, comando de teste. AGENTS.md vira ponteiro para o CLAUDE.md (uma autoridade só).
- **Menor correção segura:** adicionar a seção ao CLAUDE.md existente (~20 linhas).
- **Validação:** sessão de IA nova consegue responder "posso usar urlopen aqui?" só com o documento.

---

## 2. Acertos do server (manter — com evidência)

| ID | Acerto | Evidência |
|----|--------|-----------|
| SA-1 | Tool Gateway com pipeline de veto em 6 estágios (catalog→schema→política de estado FSM→risco→políticas especiais→exec), audit_log sempre populado, modo sandbox, "never raises" | `internal/agent/tools/gateway.py:1–40` |
| SA-2 | Segurança ops exemplar p/ produto local: token gerado com `secrets.token_hex`, comparação timing-safe, POST obrigatório, bind 127.0.0.1, allowlist | `internal/ops/security.py` (íntegra) |
| SA-3 | Config nunca expõe secrets — só booleans `*_configured`; keys lidas de env no ponto de uso | `config.py:123,311–312,348`; `llm.py:792–822` |
| SA-4 | Circuit breaker por provider LLM com estados e reset | `internal/agent/circuit_breaker.py:8–73` |
| SA-5 | Padrão async correto onde foi lembrado: `to_thread` em web_search, tools, persona | `web_search.py:472–475`; `orchestrator.py:973,1215` |
| SA-6 | Testes de contrato firmware↔server (bridge, wake, audio v2, visual) — protegem a fronteira mais frágil do sistema | `tests/test_firmware_*_contract.py` |
| SA-7 | Offline-first real: intents locais PT-BR determinísticos respondem antes/sem LLM | `intents.py:485+`; `orchestrator.py:12–21` |
| SA-8 | Composition root limpo com providers injetáveis (STT/LLM/TTS) e supervisor de conexão | `app.py:1–60` |
| SA-9 | TTS/subprocess via `asyncio.create_subprocess_exec` (não bloqueia, sem shell) | `internal/agent/tts.py:170–292` |
| SA-10 | Fake firmware p/ desenvolvimento sem hardware | `internal/debug/fake_firmware.py` |

**Acertos evolutivos:** SA-1 → persistir audit_log das tools em disco (hoje só em memória/log?) para auditoria de longo prazo; SA-6 → rodar os contratos em CI (SF-10); SA-4 → expor estado do breaker no dashboard.

---

## 3. Ordem de execução sugerida (server)

1. **SF-01** (1 linha + teste) e **SF-03** (2 linhas) — corrigem o bug quente e destravam a suite em qualquer Python.
2. **SF-10** CI mínimo — passa a proteger tudo o mais.
3. **SF-02** token no HELLO — coordenar com o R1/R2 do firmware (mesma família do F12).
4. **SF-12** autoridade do server no CLAUDE.md — barato, multiplicador.
5. **SF-06/SF-08** higiene de árvore e logs.
6. **SF-04/SF-05/SF-07** refactors guiados pelos testes de fachada.

**Síntese comparativa:** o server está em estado sensivelmente melhor que o firmware — segurança local correta, padrões async majoritariamente certos, testes de contrato e gateway de tools com veto. Os problemas são de disciplina pontual (I/O bloqueante esquecido, metadado de Python), crescimento (orchestrator) e governança (nenhuma autoridade escrita cobre este lado do repo).
