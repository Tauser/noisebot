"""noisebot_server.internal.ops.dashboard — HTML do painel de operação local.

Página auto-contida (sem dependências externas) servida em GET /.
Todo o polling é feito via fetch() para a mesma origem.
"""
from __future__ import annotations

DASHBOARD_HTML = """<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>NoiseBot Server · Dashboard</title>
<style>
:root {
  --bg:#f0f2f6; --card:#fff; --border:#dde1e7; --text:#1e293b;
  --muted:#64748b; --mono:'Consolas','Courier New',monospace;
  --accent:#3b82f6; --ok:#16a34a; --warn:#d97706; --err:#dc2626;
  --off:#94a3b8; --r:6px; --sh:0 1px 4px rgba(0,0,0,.1);
}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,'Segoe UI',sans-serif;font-size:13px;
  background:var(--bg);color:var(--text);line-height:1.5}
a{color:var(--accent)}
h2{font-size:12px;font-weight:600;text-transform:uppercase;letter-spacing:.06em;
  color:var(--muted);margin-bottom:8px}
h3{font-size:13px;font-weight:600;margin-bottom:6px}

/* Layout */
#app{max-width:1200px;margin:0 auto;padding:12px}
.header{display:flex;align-items:center;gap:10px;padding:10px 14px;
  background:var(--card);border-radius:var(--r);box-shadow:var(--sh);
  margin-bottom:12px;flex-wrap:wrap}
.header-title{font-size:15px;font-weight:700;color:var(--text);margin-right:6px}
.spacer{flex:1}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:12px}
@media(max-width:720px){.grid2{grid-template-columns:1fr}}
.card{background:var(--card);border-radius:var(--r);box-shadow:var(--sh);
  padding:14px;margin-bottom:12px}

/* Status indicators */
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;
  margin-right:4px;vertical-align:middle}
.dot-ok{background:var(--ok)} .dot-warn{background:var(--warn)}
.dot-err{background:var(--err)} .dot-off{background:var(--off)}
.pill{display:inline-block;padding:1px 7px;border-radius:99px;font-size:11px;
  font-weight:600;letter-spacing:.02em}
.pill-ok{background:#dcfce7;color:#15803d}
.pill-warn{background:#fef3c7;color:#b45309}
.pill-err{background:#fee2e2;color:#b91c1c}
.pill-off{background:#f1f5f9;color:#475569}

/* Status grid */
.sg{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:8px;margin-bottom:10px}
.sg-cell{background:#f8fafc;border:1px solid var(--border);border-radius:var(--r);
  padding:8px 10px}
.sg-label{font-size:10px;font-weight:600;text-transform:uppercase;
  letter-spacing:.06em;color:var(--muted);margin-bottom:2px}
.sg-value{font-family:var(--mono);font-size:12px;font-weight:600;color:var(--text)}

/* Provider status row */
.psr{display:flex;gap:12px;flex-wrap:wrap;margin-top:8px}
.psr-item{display:flex;align-items:center;gap:5px;font-size:12px}
.turn-detail{display:none;flex-direction:column;gap:6px;margin-top:10px}
.turn-box{background:#f8fafc;border:1px solid var(--border);border-radius:var(--r);
  padding:8px 10px}
.turn-text{font-size:12px;color:var(--text);white-space:pre-wrap;word-break:break-word}
.turn-reply{max-height:170px;overflow:auto}
.voice-alert{border:1px solid var(--border);border-radius:var(--r);padding:9px 10px;
  margin-bottom:10px;background:#f8fafc}
.voice-alert.warn{background:#fffbeb;border-color:#fde68a;color:#92400e}
.voice-alert.error{background:#fff5f5;border-color:#fecaca;color:#991b1b}
.voice-kv{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:6px}
.voice-history{margin-top:10px;display:flex;flex-direction:column;gap:5px}
.voice-row{display:grid;grid-template-columns:48px 1fr 82px 82px;gap:6px;align-items:center;
  border-bottom:1px solid #f1f5f9;padding:4px 0;font-size:11px}
.voice-row:last-child{border-bottom:none}
.voice-row span{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}

/* Metrics table */
table{width:100%;border-collapse:collapse;font-size:12px}
th{text-align:left;font-size:10px;font-weight:600;text-transform:uppercase;
  letter-spacing:.06em;color:var(--muted);padding:5px 8px;
  border-bottom:2px solid var(--border)}
td{padding:5px 8px;border-bottom:1px solid #f1f5f9;font-family:var(--mono)}
td:first-child{font-family:inherit;color:var(--text)}
tr:last-child td{border-bottom:none}
.num-good{color:var(--ok)} .num-warn{color:var(--warn)} .num-na{color:var(--muted)}

/* Turn counters */
.tc{display:flex;flex-wrap:wrap;gap:6px;margin-top:10px}
.tc-chip{background:#f1f5f9;border-radius:4px;padding:3px 8px;font-size:11px}
.tc-chip b{font-family:var(--mono)}

/* Errors */
.err-list{display:flex;flex-direction:column;gap:6px}
.err-item{background:#fff5f5;border:1px solid #fecaca;border-radius:var(--r);
  padding:8px 10px;font-size:12px}
.err-meta{color:var(--muted);font-size:11px;margin-top:3px;font-family:var(--mono)}
.empty-state{color:var(--muted);font-size:12px;text-align:center;padding:12px}

/* Config */
.form-row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:8px}
label{font-size:12px;font-weight:600;color:var(--muted);min-width:80px}
select,input[type=number],input[type=text],input[type=password]{
  border:1px solid var(--border);border-radius:4px;padding:4px 8px;
  font-size:12px;background:#fff;color:var(--text);outline:none;
  font-family:inherit}
select:focus,input:focus{border-color:var(--accent);box-shadow:0 0 0 2px rgba(59,130,246,.15)}
.warn-box{background:#fffbeb;border:1px solid #fde68a;border-radius:var(--r);
  padding:8px 10px;font-size:12px;color:#92400e;margin-top:8px}

/* Buttons */
.btn{display:inline-flex;align-items:center;gap:4px;padding:5px 12px;
  border-radius:4px;font-size:12px;font-weight:600;cursor:pointer;
  border:1px solid transparent;transition:opacity .15s}
.btn:disabled{opacity:.4;cursor:not-allowed}
.btn-primary{background:var(--accent);color:#fff;border-color:var(--accent)}
.btn-primary:hover:not(:disabled){opacity:.85}
.btn-secondary{background:#f1f5f9;color:var(--text);border-color:var(--border)}
.btn-secondary:hover:not(:disabled){background:#e2e8f0}
.btn-danger{background:#fee2e2;color:var(--err);border-color:#fecaca}
.btn-danger:hover:not(:disabled){background:#fecaca}
.btn-warn{background:#fef3c7;color:#b45309;border-color:#fde68a}
.btn-warn:hover:not(:disabled){background:#fde68a}
.btn-row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}

/* Token */
.token-section{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.token-status{font-size:11px;font-weight:600}
.token-ok{color:var(--ok)} .token-none{color:var(--err)}

/* Tests */
.test-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:8px}
.test-card{border:1px solid var(--border);border-radius:var(--r);padding:10px;
  background:#fafafa}
.test-card h4{font-size:12px;font-weight:600;margin-bottom:4px}
.test-card p{font-size:11px;color:var(--muted);margin-bottom:6px}

/* Raw JSON */
details{margin-top:4px}
summary{cursor:pointer;font-size:11px;color:var(--muted);
  font-weight:600;text-transform:uppercase;letter-spacing:.05em;
  user-select:none;padding:4px 0}
pre{background:#1e293b;color:#94a3b8;border-radius:var(--r);
  padding:10px;font-size:11px;overflow:auto;max-height:300px;
  font-family:var(--mono);margin-top:6px}

/* Age indicator */
.age{font-size:11px;color:var(--muted);margin-left:4px}
.age-stale{color:var(--warn)}

/* Overlay for confirmations */
.confirm-overlay{position:fixed;inset:0;background:rgba(0,0,0,.35);
  display:flex;align-items:center;justify-content:center;z-index:100}
.confirm-box{background:#fff;border-radius:var(--r);padding:20px 24px;
  max-width:360px;box-shadow:0 8px 32px rgba(0,0,0,.18)}
.confirm-box h3{margin-bottom:8px}
.confirm-box p{color:var(--muted);font-size:13px;margin-bottom:16px}
.hidden{display:none}

/* Toast */
#toast{position:fixed;bottom:20px;right:20px;background:#1e293b;color:#fff;
  padding:8px 14px;border-radius:var(--r);font-size:12px;
  transition:opacity .3s;pointer-events:none}
</style>
</head>
<body>
<div id="app">

<!-- Header -->
<div class="header">
  <span class="header-title">NoiseBot Server</span>
  <span id="hc-dot" class="dot dot-off" title="Process health"></span>
  <span id="hc-label" style="font-size:12px;color:var(--muted)">—</span>
  <span id="uptime" class="age"></span>
  <div class="spacer"></div>
  <span id="last-update" class="age"></span>
  <label style="display:flex;align-items:center;gap:5px;font-size:12px;font-weight:600;min-width:0">
    <input type="checkbox" id="auto-refresh" checked>
    Auto
  </label>
  <input type="number" id="refresh-interval" value="5" min="1" max="60"
    style="width:46px" title="Intervalo (s)">
  <span style="font-size:11px;color:var(--muted)">s</span>
  <button class="btn btn-secondary" onclick="doRefresh()">⟳ Reload</button>
</div>

<!-- Token -->
<div class="card">
  <h2>Autenticação</h2>
  <div class="token-section">
    <input type="password" id="token-input" placeholder="Cole o token de ~/.noisebot-server/ops_token"
      style="flex:1;min-width:200px;max-width:360px">
    <button class="btn btn-primary" onclick="saveToken()">Salvar Token</button>
    <button class="btn btn-secondary" onclick="clearToken()">Limpar</button>
    <span id="token-status" class="token-status token-none">⚠ Sem token — POSTs falharão</span>
  </div>
  <p style="margin-top:6px;font-size:11px;color:var(--muted)">
    Encontre o token em <code style="font-family:var(--mono)">~/.noisebot-server/ops_token</code>
    ou defina <code style="font-family:var(--mono)">NOISEBOT_OPS_TOKEN</code> no ambiente.
    O token nunca é exibido após salvo.
  </p>
</div>

<!-- Main grid -->
<div class="grid2">

  <!-- Left: Status + Errors -->
  <div>
    <div class="card">
      <h2>Status</h2>
      <div class="sg" id="status-grid">
        <!-- preenchido por JS -->
      </div>
      <div class="psr" id="provider-status"></div>
      <div id="last-turn" style="margin-top:8px;font-size:12px;color:var(--muted)"></div>
      <div id="turn-detail" class="turn-detail">
        <div class="turn-box">
          <div class="sg-label">Você disse</div>
          <div id="turn-transcript" class="turn-text">—</div>
        </div>
        <div class="turn-box">
          <div class="sg-label">Resposta da IA</div>
          <div id="turn-reply" class="turn-text turn-reply">—</div>
        </div>
      </div>
      <div id="last-error-box" style="margin-top:6px;display:none">
        <div class="pill pill-err" id="last-error-text"></div>
      </div>
    </div>

    <div class="card">
      <h2>Erros Recentes</h2>
      <div id="errors-list" class="err-list">
        <p class="empty-state">Nenhum erro recente.</p>
      </div>
    </div>
  </div>

  <!-- Right: Metrics + Actions -->
  <div>
    <div class="card">
      <div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:8px">
        <h2 style="margin-bottom:0">Métricas de Latência</h2>
        <button class="btn btn-secondary" onclick="resetMetrics()" id="btn-reset-metrics">
          ↺ Reset
        </button>
      </div>
      <table>
        <thead>
          <tr><th>Métrica</th><th>p50</th><th>p95</th></tr>
        </thead>
        <tbody id="metrics-tbody">
          <tr><td colspan="3" class="num-na">—</td></tr>
        </tbody>
      </table>
      <div class="tc" id="turn-counters"></div>
      <div id="token-usage" style="margin-top:8px;font-size:11px;color:var(--muted)"></div>
    </div>

    <div class="card">
      <h2>Diagnóstico de Voz</h2>
      <div id="voice-diagnosis">
        <p class="empty-state">Nenhuma sessão de voz registrada.</p>
      </div>
    </div>

    <div class="card">
      <h2>Configuração de IA</h2>
      <div class="form-row">
        <label>Provider</label>
        <select id="cfg-provider" onchange="onProviderChange()">
          <option value="openai">openai</option>
          <option value="gemini">gemini</option>
          <option value="ollama">ollama</option>
          <option value="none">none</option>
        </select>
      </div>
      <div class="form-row">
        <label>Modelo</label>
        <select id="cfg-model"></select>
      </div>
      <div class="form-row">
        <label>Max tokens</label>
        <input type="number" id="cfg-max-tokens" value="256" min="1" max="4096" style="width:80px">
      </div>
      <div class="btn-row" style="margin-top:6px">
        <button class="btn btn-primary" onclick="applyConfig()" id="btn-apply-cfg">
          Aplicar Config
        </button>
        <span id="cfg-result" style="font-size:11px;color:var(--muted)"></span>
      </div>
      <hr style="margin:10px 0;border:none;border-top:1px solid var(--border)">
      <div class="form-row">
        <label>Modo</label>
        <select id="cfg-mode">
          <option value="normal">normal</option>
          <option value="local_only">local_only</option>
          <option value="degraded">degraded</option>
          <option value="realtime">realtime (futuro)</option>
        </select>
        <button class="btn btn-primary" onclick="applyMode()" id="btn-apply-mode">
          Aplicar Modo
        </button>
      </div>
      <div class="warn-box">
        🔒 API keys nunca aqui. Use variáveis de ambiente ou arquivo
        <code style="font-family:var(--mono)">.env</code> fora do repositório.
      </div>
    </div>

    <div class="card">
      <h2>Ações Operacionais</h2>
      <div class="btn-row">
        <button class="btn btn-secondary" onclick="doRefresh()">⟳ Recarregar</button>
        <button class="btn btn-warn" onclick="confirmRestart()" id="btn-restart">
          ⚡ Reinício Gracioso
        </button>
        <button class="btn btn-secondary" onclick="copyDiagnostic()">
          📋 Copiar JSON
        </button>
      </div>
    </div>
  </div>
</div><!-- /grid2 -->

<!-- Tests section -->
<div class="card">
  <h2>Testes Manuais</h2>
  <div class="test-grid">
    <div class="test-card">
      <h4>Inject Transcript</h4>
      <p>Injeta FinalTranscript sintético no orchestrator para testar pipeline sem microfone.</p>
      <input type="text" id="debug-transcript-text" value="que horas são"
        style="width:100%;margin-bottom:6px" maxlength="500">
      <button class="btn btn-primary" onclick="runTranscriptTest()">
        Rodar Transcript
      </button>
    </div>
    <div class="test-card">
      <h4>Voice Turn Sintético</h4>
      <p>Simula VOICE_START + áudio silencioso + VOICE_END para exercitar o caminho de STT.</p>
      <div class="form-row" style="margin-bottom:6px">
        <label style="min-width:54px">Chunks</label>
        <input type="number" id="debug-voice-chunks" value="40" min="1" max="300" style="width:80px">
      </div>
      <button class="btn btn-primary" onclick="runVoiceTurnTest()">
        Rodar Voice Turn
      </button>
    </div>
    <div class="test-card">
      <h4>Fake Firmware</h4>
      <p>O simulador ainda é processo separado; mantenha-o rodando quando quiser testar protocolo TCP.</p>
      <button class="btn btn-secondary" disabled>
        Processo separado
      </button>
      <p style="font-size:10px;color:var(--muted);margin-top:4px">
        <code style="font-family:var(--mono)">python -m noisebot_server debug fake-fw --port 9001</code>
      </p>
    </div>
    <div class="test-card">
      <h4>Replay de Áudio</h4>
      <p>Replay de WAV/PCM gravado sem precisar do firmware real.</p>
      <button class="btn btn-secondary" disabled>
        Próximo endpoint
      </button>
    </div>
  </div>
</div>

<!-- Raw JSON diagnostic -->
<div class="card">
  <details id="raw-details">
    <summary>▶ Diagnóstico JSON Bruto</summary>
    <pre id="raw-json">Nenhum dado ainda. Clique em Reload.</pre>
  </details>
</div>

</div><!-- /#app -->

<!-- Confirm overlay -->
<div id="confirm-overlay" class="confirm-overlay hidden">
  <div class="confirm-box">
    <h3>⚡ Reinício Gracioso</h3>
    <p>O bridge aguardará o turno em andamento terminar e então reiniciará.
       O dashboard ficará offline brevemente.</p>
    <div class="btn-row">
      <button class="btn btn-danger" onclick="doRestart()">Confirmar Reinício</button>
      <button class="btn btn-secondary" onclick="hideConfirm()">Cancelar</button>
    </div>
  </div>
</div>

<div id="toast" class="hidden"></div>

<script>
'use strict';

// ── Catálogo de providers (espelha ops/config.py) ─────────────────────────
const CATALOG = {
  openai: ['gpt-4o-mini','gpt-4o','gpt-4.1-mini','gpt-4.1','gpt-4-turbo'],
  gemini: ['gemini-2.5-flash','gemini-2.5-flash-lite','gemini-2.5-pro'],
  ollama: ['qwen3.5:9b','qwen2.5:7b','qwen2.5:14b','llama3.1:8b','llama3.2:3b','mistral:7b'],
  none:   [],
};

// ── Estado global ─────────────────────────────────────────────────────────
let state = { health:null, status:null, metrics:null, errors:null, config:null };
let lastFetch = null;
let refreshTimer = null;
let rawSnapshot = {};

// ── Token ─────────────────────────────────────────────────────────────────
function getToken()    { return localStorage.getItem('nb_ops_token') || ''; }
function saveToken()   {
  const v = document.getElementById('token-input').value.trim();
  if (!v) { showToast('Token vazio — não salvo.'); return; }
  localStorage.setItem('nb_ops_token', v);
  document.getElementById('token-input').value = '';
  updateTokenStatus();
  showToast('Token salvo em localStorage.');
}
function clearToken()  {
  localStorage.removeItem('nb_ops_token');
  updateTokenStatus();
  showToast('Token removido.');
}
function updateTokenStatus() {
  const el = document.getElementById('token-status');
  const has = !!getToken();
  el.className = 'token-status ' + (has ? 'token-ok' : 'token-none');
  el.textContent = has ? '✓ Token configurado' : '⚠ Sem token — POSTs falharão';
}

// ── Fetch helper ──────────────────────────────────────────────────────────
async function api(path, opts = {}) {
  const token = getToken();
  const headers = { 'Content-Type': 'application/json', ...(opts.headers||{}) };
  if (token) headers['Authorization'] = 'Bearer ' + token;
  const r = await fetch(path, { ...opts, headers });
  if (!r.ok) {
    const body = await r.json().catch(() => ({ error: r.statusText }));
    throw Object.assign(new Error(body.error || r.statusText), { status: r.status, body });
  }
  return r.json();
}

// ── Refresh ───────────────────────────────────────────────────────────────
async function doRefresh() {
  try {
    const [health, status, metrics, errors, config] = await Promise.all([
      api('/health'),
      api('/ai/status'),
      api('/ai/metrics'),
      api('/ai/errors'),
      api('/ai/config'),
    ]);
    state = { health, status, metrics, errors, config };
    rawSnapshot = { health, status, metrics, errors };  // config omitido (pode ter info)
    lastFetch = Date.now();
    render();
  } catch (e) {
    renderOffline(e.message);
  }
}

function scheduleRefresh() {
  if (refreshTimer) clearInterval(refreshTimer);
  const auto = document.getElementById('auto-refresh').checked;
  if (!auto) return;
  const iv = Math.max(1, parseInt(document.getElementById('refresh-interval').value, 10)) * 1000;
  refreshTimer = setInterval(doRefresh, iv);
}

document.getElementById('auto-refresh').addEventListener('change', scheduleRefresh);
document.getElementById('refresh-interval').addEventListener('change', scheduleRefresh);

// ── Render ────────────────────────────────────────────────────────────────
function render() {
  const { health, status, metrics, errors, config } = state;
  renderHeader(health);
  if (status) renderStatus(status);
  if (metrics) renderMetrics(metrics);
  if (metrics) renderVoiceDiagnostics(metrics);
  if (errors)  renderErrors(errors);
  if (config)  populateConfigForm(config);
  if (document.getElementById('raw-details').open) renderRaw();
  updateAge();
}

function renderOffline(msg) {
  const dot = document.getElementById('hc-dot');
  dot.className = 'dot dot-off';
  document.getElementById('hc-label').textContent = 'Offline: ' + (msg||'erro de conexão');
}

function renderHeader(health) {
  if (!health) return;
  const dot = document.getElementById('hc-dot');
  const lbl = document.getElementById('hc-label');
  const up  = document.getElementById('uptime');
  const ok  = health.status === 'ok';
  dot.className = 'dot ' + (ok ? 'dot-ok' : 'dot-warn');
  lbl.textContent = ok ? 'Healthy' : 'Degraded';
  up.textContent = health.uptime_s != null ? '↑ ' + fmtUptime(health.uptime_s) : '';
}

function renderStatus(s) {
  const grid = document.getElementById('status-grid');
  const connected = s.connected;
  grid.innerHTML = [
    cell('Firmware', connected
      ? '<span class="pill pill-ok">Conectado</span>'
      : '<span class="pill pill-off">Desconectado</span>'),
    cell('Pipeline', s.pipeline || '—'),
    cell('Modo', modePill(s.mode)),
    cell('Provider', s.provider || '—'),
    cell('Modelo', truncate(s.model, 16) || '—'),
    cell('API Key', s.api_key_configured
      ? '<span class="pill pill-ok">Configurada</span>'
      : '<span class="pill pill-warn">Ausente</span>'),
  ].join('');

  const psr = document.getElementById('provider-status');
  psr.innerHTML = [
    providerPill('STT', s.stt_status),
    providerPill('LLM', s.llm_status),
    providerPill('TTS', s.tts_status),
  ].join('');

  const lt = document.getElementById('last-turn');
  if (s.last_turn_id) {
    lt.innerHTML = 'Turno <b>#' + s.last_turn_id + '</b> · ' +
      outcomePill(s.last_outcome) +
      (s.last_route ? '<span class="age"> · rota: ' + safeStr(s.last_route) + '</span>' : '') +
      '<span class="age"> · ' + (s.updated_at || '') + '</span>';
  } else {
    lt.textContent = '';
  }

  const td = document.getElementById('turn-detail');
  const transcript = s.last_transcript || '';
  const reply = s.last_reply || '';
  if (s.last_turn_id && (transcript || reply)) {
    td.style.display = 'flex';
    document.getElementById('turn-transcript').textContent = transcript || '—';
    document.getElementById('turn-reply').textContent = reply || '—';
  } else {
    td.style.display = 'none';
  }

  const eb = document.getElementById('last-error-box');
  if (s.last_error) {
    eb.style.display = '';
    document.getElementById('last-error-text').textContent =
      'Último erro: ' + safeStr(s.last_error.kind) +
      (s.last_error.provider ? ' (' + s.last_error.provider + ')' : '');
  } else {
    eb.style.display = 'none';
  }
}

function renderMetrics(m) {
  const tbody = document.getElementById('metrics-tbody');
  const lat = m.latency_ms || {};
  const rows = [
    ['STT',                'stt'],
    ['LLM first token',    'llm_first_token'],
    ['LLM total',          'llm_total'],
    ['TTS first audio',    'tts_first_audio'],
    ['First audio out',    'first_audio_out'],
    ['First robot react',  'first_robot_reaction'],
    ['Interruption cancel','interruption_cancel'],
  ];
  tbody.innerHTML = rows.map(([label, key]) => {
    const e = lat[key];
    if (!e) return '<tr><td>' + label + '</td><td class="num-na">—</td><td class="num-na">—</td></tr>';
    return '<tr><td>' + label + '</td>' +
      '<td class="' + latClass(e.p50, key) + '">' + fmtMs(e.p50) + '</td>' +
      '<td class="' + latClass(e.p95, key) + '">' + fmtMs(e.p95) + '</td>' +
      '</tr>';
  }).join('');

  const tc = document.getElementById('turn-counters');
  const t = m.turns || {};
  tc.innerHTML = Object.entries(t).map(([k,v]) =>
    '<span class="tc-chip">' + k + ' <b>' + v + '</b></span>'
  ).join('');

  const tok = document.getElementById('token-usage');
  if (m.tokens && (m.tokens.input || m.tokens.output)) {
    tok.textContent = 'Tokens — entrada: ' + (m.tokens.input||0) + ' · saída: ' + (m.tokens.output||0);
  }
}

function renderVoiceDiagnostics(m) {
  const root = document.getElementById('voice-diagnosis');
  const session = m.last_voice_session || {};
  if (!session.turn_id) {
    root.innerHTML = '<p class="empty-state">Nenhuma sessão de voz registrada.</p>';
    return;
  }

  const alert = m.voice_alert || null;
  const diagnosis = m.voice_diagnosis || {};
  const level = alert ? (alert.level === 'error' ? 'error' : 'warn') : '';
  const title = diagnosis.title || (alert ? alert.title : 'Turno de voz concluído');
  const detail = diagnosis.detail || (alert ? alert.detail : 'sem alerta');
  const next = diagnosis.next_check || 'Comparar métricas do turno com o áudio real.';
  const history = (m.recent_voice_sessions || []).slice(0, 5);

  root.innerHTML =
    '<div class="voice-alert ' + level + '">' +
      '<h3>' + safeStr(title) + '</h3>' +
      '<div>' + safeStr(detail) + '</div>' +
      '<div class="err-meta">' + safeStr(next) + '</div>' +
    '</div>' +
    '<div class="voice-kv">' +
      cell('Turno', '#' + safeStr(session.turn_id)) +
      cell('Resultado', outcomePill(session.outcome || '—')) +
      cell('Fim', safeStr(session.voice_end_reason || session.discard_reason || '—')) +
      cell('Duração', fmtValueMs(session.duration_ms)) +
      cell('Chunks', safeStr(session.chunk_count ?? '—')) +
      cell('Amostras', safeStr(session.total_samples ?? '—')) +
      cell('Qualidade STT', safeStr(session.transcript_quality || '—')) +
      cell('Política', safeStr(session.turn_taking_policy || '—')) +
      cell('Decisão', safeStr(session.turn_taking_decision || '—')) +
      cell('Voz → STT', fmtValueMs(session.voice_end_to_stt_start_ms)) +
      cell('STT', fmtValueMs(session.stt_ms)) +
      cell('1º áudio', fmtValueMs(session.first_audio_out_ms)) +
      cell('1º após voz', fmtValueMs(session.first_audio_after_voice_end_ms)) +
      cell('Fala total', fmtValueMs(session.speech_total_ms)) +
    '</div>' +
    '<div class="turn-detail" style="display:flex">' +
      '<div class="turn-box"><div class="sg-label">Transcrição</div><div class="turn-text">' +
        safeStr(session.transcript || '—') + '</div></div>' +
      '<div class="turn-box"><div class="sg-label">Resposta</div><div class="turn-text turn-reply">' +
        safeStr(session.reply || '—') + '</div></div>' +
    '</div>' +
    '<div class="voice-history">' +
      '<div class="sg-label">Histórico recente</div>' +
      (history.length ? history.map(voiceHistoryRow).join('') : '<p class="empty-state">Sem histórico.</p>') +
    '</div>';
}

function renderErrors(data) {
  const list = document.getElementById('errors-list');
  const errs = (data.errors || []).slice(0, 10);
  if (!errs.length) {
    list.innerHTML = '<p class="empty-state">Nenhum erro recente.</p>';
    return;
  }
  list.innerHTML = errs.map(e => {
    const ts = e.ts ? new Date(e.ts * 1000).toLocaleTimeString('pt-BR') : '—';
    return '<div class="err-item">' +
      '<b>' + safeStr(e.kind) + '</b>' +
      (e.provider ? ' · ' + safeStr(e.provider) : '') +
      (e.message  ? ' · ' + safeStr(e.message)  : '') +
      '<div class="err-meta">turn #' + (e.turn_id||'?') + ' · ' + ts + '</div>' +
      '</div>';
  }).join('');
}

function populateConfigForm(cfg) {
  const llm = cfg.llm || {};
  const p = llm.provider || 'openai';
  const sel = document.getElementById('cfg-provider');
  if (sel.value !== p) sel.value = p;
  populateModels(p, llm.model);
  const mt = document.getElementById('cfg-max-tokens');
  if (!mt.matches(':focus') && llm.max_output_tokens) mt.value = llm.max_output_tokens;
  const ms = document.getElementById('cfg-mode');
  if (cfg.pipeline_mode) ms.value = cfg.pipeline_mode;
}

function onProviderChange() {
  populateModels(document.getElementById('cfg-provider').value, null);
}

function populateModels(provider, current) {
  const sel = document.getElementById('cfg-model');
  const models = CATALOG[provider] || [];
  sel.innerHTML = models.length
    ? models.map(m => '<option value="' + m + '">' + m + '</option>').join('')
    : '<option value="">— nenhum modelo —</option>';
  if (current && models.includes(current)) sel.value = current;
}

function renderRaw() {
  document.getElementById('raw-json').textContent = JSON.stringify(rawSnapshot, null, 2);
}

// ── Actions ───────────────────────────────────────────────────────────────
async function applyConfig() {
  const provider  = document.getElementById('cfg-provider').value;
  const model     = document.getElementById('cfg-model').value;
  const maxTokens = parseInt(document.getElementById('cfg-max-tokens').value, 10);
  const result    = document.getElementById('cfg-result');
  result.textContent = 'Aplicando…';
  try {
    const data = {};
    if (provider) data.provider = provider;
    if (model)    data.model    = model;
    if (maxTokens) data.max_tokens = maxTokens;
    await api('/ai/config', { method:'POST', body: JSON.stringify(data) });
    result.style.color = 'var(--ok)';
    result.textContent = '✓ Configuração aplicada.';
    showToast('Config aplicada.');
    setTimeout(doRefresh, 800);
  } catch (e) {
    result.style.color = 'var(--err)';
    result.textContent = '✗ ' + (e.body?.error || e.message);
  }
}

async function applyMode() {
  const mode = document.getElementById('cfg-mode').value;
  try {
    await api('/ai/mode', { method:'POST', body: JSON.stringify({ mode }) });
    showToast('Modo alterado para: ' + mode);
    setTimeout(doRefresh, 600);
  } catch (e) {
    showToast('✗ ' + (e.body?.error || e.message), true);
  }
}

async function resetMetrics() {
  try {
    await api('/ai/metrics/reset', { method:'POST' });
    showToast('Métricas zeradas.');
    setTimeout(doRefresh, 400);
  } catch (e) {
    showToast('✗ ' + (e.body?.error || e.message), true);
  }
}

async function runTranscriptTest() {
  const input = document.getElementById('debug-transcript-text');
  const text = input.value.trim();
  if (!text) {
    showToast('Digite um texto para injetar.', true);
    return;
  }
  try {
    const data = await api('/debug/transcript', {
      method:'POST',
      body: JSON.stringify({ text }),
    });
    showToast('Transcript injetado: turno #' + data.turn_id);
    setTimeout(doRefresh, 900);
  } catch (e) {
    showToast('✗ ' + (e.body?.error || e.message), true);
  }
}

async function runVoiceTurnTest() {
  const chunks = parseInt(document.getElementById('debug-voice-chunks').value, 10) || 40;
  try {
    await api('/debug/voice-turn', {
      method:'POST',
      body: JSON.stringify({ chunks }),
    });
    showToast('Voice turn sintético injetado.');
    setTimeout(doRefresh, 1200);
  } catch (e) {
    showToast('✗ ' + (e.body?.error || e.message), true);
  }
}

function confirmRestart() {
  document.getElementById('confirm-overlay').classList.remove('hidden');
}
function hideConfirm() {
  document.getElementById('confirm-overlay').classList.add('hidden');
}
async function doRestart() {
  hideConfirm();
  try {
    await api('/ai/restart', { method:'POST' });
    showToast('Reinício gracioso agendado. O servidor ficará offline em breve.');
    setTimeout(doRefresh, 3000);
  } catch (e) {
    showToast('✗ ' + (e.body?.error || e.message), true);
  }
}

function copyDiagnostic() {
  const text = JSON.stringify(rawSnapshot, null, 2);
  navigator.clipboard.writeText(text).then(
    () => showToast('JSON copiado para a área de transferência.'),
    () => showToast('Falha ao copiar — abra o painel Raw.', true),
  );
}

// ── Helpers ───────────────────────────────────────────────────────────────
function cell(label, value) {
  return '<div class="sg-cell"><div class="sg-label">' + label +
    '</div><div class="sg-value">' + value + '</div></div>';
}

function modePill(mode) {
  const cls = { normal:'pill-ok', local_only:'pill-warn', degraded:'pill-warn',
                realtime:'pill-ok' }[mode] || 'pill-off';
  return '<span class="pill ' + cls + '">' + (mode||'—') + '</span>';
}

function outcomePill(o) {
  const cls = { ok:'pill-ok', local_intent:'pill-ok', llm:'pill-ok',
                fallback:'pill-warn', failed:'pill-err', interrupted:'pill-warn' }[o] || 'pill-off';
  return '<span class="pill ' + cls + '">' + (o||'—') + '</span>';
}

function providerPill(label, st) {
  const cls = { ok:'pill-ok', degraded:'pill-warn', unavailable:'pill-err' }[st] || 'pill-off';
  return '<span class="psr-item"><b>' + label + '</b> <span class="pill ' + cls + '">' +
    (st||'—') + '</span></span>';
}

function fmtMs(v) {
  if (v == null) return '<span class="num-na">—</span>';
  return Math.round(v) + ' ms';
}

function fmtValueMs(v) {
  if (v == null || v === '') return '—';
  return Math.round(Number(v)) + ' ms';
}

function voiceHistoryRow(s) {
  const reason = s.turn_taking_decision || s.discard_reason || s.intent_name || s.outcome || '';
  return '<div class="voice-row">' +
    '<span>#' + safeStr(s.turn_id || '?') + '</span>' +
    '<span title="' + safeStr(reason) + '">' + safeStr(reason || '—') + '</span>' +
    '<span>' + safeStr(s.transcript_quality || '—') + '</span>' +
    '<span>' + fmtValueMs(s.duration_ms) + '</span>' +
  '</div>';
}

const LAT_THRESHOLDS = {
  stt: [800,1500], llm_first_token: [600,1200], llm_total: [2500,5000],
  tts_first_audio: [400,800], first_audio_out: [1500,3000],
  first_robot_reaction: [300,600], interruption_cancel: [200,400],
};
function latClass(v, key) {
  if (v == null) return 'num-na';
  const t = LAT_THRESHOLDS[key];
  if (!t) return '';
  return v <= t[0] ? 'num-good' : v <= t[1] ? '' : 'num-warn';
}

function fmtUptime(s) {
  if (s < 60) return Math.round(s) + 's';
  if (s < 3600) return Math.round(s/60) + 'm';
  return Math.round(s/3600) + 'h ' + Math.round((s%3600)/60) + 'm';
}

function truncate(s, n) {
  return s && s.length > n ? s.slice(0, n) + '…' : (s||'');
}

function safeStr(s) {
  if (!s) return '';
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')
    .replace(/"/g,'&quot;').replace(/'/g,'&#39;').slice(0, 200);
}

function updateAge() {
  if (!lastFetch) return;
  const age = Math.round((Date.now() - lastFetch) / 1000);
  const el = document.getElementById('last-update');
  el.className = 'age' + (age > 15 ? ' age-stale' : '');
  el.textContent = 'atualizado ' + age + 's atrás';
}

setInterval(() => { if (lastFetch) updateAge(); }, 1000);

// ── Toast ─────────────────────────────────────────────────────────────────
let toastTimer = null;
function showToast(msg, isError = false) {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.style.background = isError ? '#991b1b' : '#1e293b';
  el.classList.remove('hidden');
  el.style.opacity = '1';
  if (toastTimer) clearTimeout(toastTimer);
  toastTimer = setTimeout(() => {
    el.style.opacity = '0';
    setTimeout(() => el.classList.add('hidden'), 300);
  }, 3000);
}

// ── Raw JSON toggle ────────────────────────────────────────────────────────
document.getElementById('raw-details').addEventListener('toggle', function() {
  if (this.open) renderRaw();
});

// ── Init ──────────────────────────────────────────────────────────────────
updateTokenStatus();
populateModels('openai', null);
doRefresh();
scheduleRefresh();
</script>
</body>
</html>"""


def get_dashboard_html() -> str:
    return DASHBOARD_HTML
