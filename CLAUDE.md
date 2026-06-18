# CLAUDE.md — NoiseBot Firmware

Instruções para o assistente de IA trabalhando neste repositório.
Este arquivo tem autoridade máxima sobre qualquer instrução geral.

---

## Workflow

**Não usar skills GSD** (`/gsd:*`) neste projeto. Trabalhar diretamente — planejamento e execução inline sem subagentes ou orchestrators GSD.

### Knowledge OS

**Decisão ativa:** não atualizar o Knowledge OS externo por enquanto (decisão do usuário, 2026-06).
O projeto usa este CLAUDE.md + `docs/ROADMAP.md` como fontes de verdade operacionais.

---

## Projeto

**NoiseBot** é um companion robot desktop expressivo baseado em ESP32-S3.
O firmware é C17 puro, salvo o componente `nb_hal/display` (C++ para LovyanGFX).
A stack é ESP-IDF + FreeRTOS. **Nunca usar Arduino.**

---

## Regras Inegociáveis

### Stack e Linguagem

- ESP-IDF (não Arduino). Toda API vem de `esp_*` ou `freertos/`.
- C17 em todos os componentes exceto `nb_hal/display` (C++ exclusivamente para LovyanGFX).
- CMake via `idf_component_register`. Não usar Makefile legado.
- Compilar com `-Wall -Wextra -Werror` — zero warnings tolerados.

### Arquitetura em Camadas

- Camadas só chamam para baixo (Layer N → Layer N-1 ou inferior).
- Comunicação entre camadas não adjacentes: **sempre via event bus**.
- Nenhum componente de comportamento (Layer 5-7) chama HAL diretamente.
- Nenhum HAL publica no event bus diretamente — passes para o serviço da Layer 4.

### Baseline de Comportamento

- `IDLE` é sempre o baseline persistente visual e comportamental do robô.
- A base de `IDLE` é expressão `NEUTRAL`, gaze central, pescoço central e LED idle.
- Expressões/ações como `CURIOUS`, `HAPPY`, `FOCUSED`, `ATTENTIVE`, touch, wake e fala são momentos transitórios ou overlays. Elas nunca substituem o baseline de `IDLE`.
- Toda entrada em `IDLE` deve limpar expressão, gaze, postura e overlays transitórios antes de aceitar novos comportamentos.
- `SLEEPING`, `MEDITATION`, `SILENT_COMPANY`, `RESPONDING` e estados de erro podem ter bases próprias, mas ao sair deles para `IDLE` o baseline de `IDLE` volta a ter autoridade.

```
Layer 0: ESP-IDF / FreeRTOS / Hardware
Layer 1: HAL        (nb_hal/display, nb_hal/servo, nb_hal/audio, nb_hal/led, nb_hal/touch, nb_hal/sd)
Layer 2: Infra      (event_bus, logger, config_manager, persistence_mgr, watchdog, boot_manager)
Layer 3: Safety     (motion_safety, power_monitor, error_policy)
Layer 4: Services   (render_service, motion_service, audio_service, led_service, touch_service)
Layer 5: Core Svcs  (gaze_service, idle_service, expression_service, conductor)
Layer 6: Behavior   (behavior_engine, state_machine, emotion_model)
Layer 7: Persona    (persona_service, long_term_memory)
Layer 8: Futuro     (camera, imu, battery)
```

### Motion Safety — Regra de Veto

- **Nenhum movimento de servo é implementado antes de `motion_safety` estar verde.**
- `motion_safety` tem autoridade de veto sobre qualquer comando de posição.
- Toda escrita de posição passa obrigatoriamente por `motion_safety_check_position()`.
- Stall detection, heartbeat timeout e brownout disable são não-negociáveis.
- Ver `docs/SERVO_SAFETY.md` para o protocolo completo de liberação.

### Memória

- **Nenhum framebuffer de display em SRAM.** Sprites LovyanGFX alocam em PSRAM.
- Buffers de áudio DMA: SRAM (verificar se DMA I2S alcança PSRAM no S3).
- Nenhum `malloc()` em caminho crítico (ISR, task de safety, render loop).
- Estruturas estáticas para event bus e pools de objetos frequentes.
- Monitorar `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` — manter >300KB livres (headroom câmera).

### Persistência e I/O

- **Nunca escrever em SD de forma síncrona em task com prioridade ≥ 10.**
- Toda escrita não-urgente vai para a fila da `persistence_task` (prioridade 5).
- Exceção: crash dump usa escrita síncrona direta (sistema já em falha).
- NVS para configuração crítica e flags de safety. SD para logs, assets, memória longa.

### Event Bus

- Ao mudar estado significativo: publicar evento no event bus.
- Nunca chamar subscriber diretamente para comunicação cross-layer.
- Eventos de safety têm fila separada e nunca são bloqueados por backpressure normal.

### WiFi e Conectividade

- **WiFi ativo em produção** via `wifi_service` (boot-time, background, Layer 2). A Etapa 9.6 formaliza o serviço, mas WiFi já está habilitado desde o ciclo atual.
- O produto é **offline-first**: funciona 100% sem WiFi. Conectividade é conveniência, nunca dependência.
- Sem TLS/HTTPS no firmware: mbedTLS ~250 KB SRAM — inviável. HTTP local apenas.
- Endpoints mutadores (OTA, restart, config POST) exigem header `X-NB-Token` — token gerado no primeiro boot e logado no console (NVS, chave `api_token`).
- **Bridge firmware↔server (SF-02):** o HELLO do bridge (porta TCP 9000) pode
  carregar o mesmo `api_token` (NVS `nb_sys/api_token`) no campo `"token"`.
  Validação no firmware é opt-in via flag NVS `nb_sys/bridge_req_tok` (default
  `0` = desabilitada, retrocompatível). Quando habilitada, HELLO sem token ou
  com token incorreto faz o firmware encerrar a conexão TCP sem responder.
  No server, o token é somente-leitura: copiar o valor logado pelo firmware
  para `NOISEBOT_BRIDGE_TOKEN` ou `~/.noisebot-server/bridge_token`
  (`protocol.load_bridge_token()` / `server_hello_capabilities()`).

---

## Estrutura de Componentes

```
components/
├── infra/          # Layer 2+3: boot_manager, logger, event_bus,
│                   #            config_manager, nvs_hal, persistence_mgr,
│                   #            watchdog_service, error_policy, nb_events.h,
│                   #            nb_task_config.h, nb_config_keys.h,
│                   #            motion_safety, power_monitor (Layer 3 físico),
│                   #            web_service, web_ota, bridge_service,
│                   #            wifi_service, diagnostics_service
├── nb_hal/         # Layer 1: display_hal (.cpp + .h), servo_hal,
│                   #          audio_hal, led_hal, touch_hal, sd_hal,
│                   #          i2c_hal, camera_hal (preparado; DVP reservado),
│                   #          board_caps, nb_hw_config.h
├── services/       # Layer 4-5: render_service, motion_service,
│                   #             audio_service (*_v2 canônico), led_service,
│                   #             touch_service, touch_semantic_service,
│                   #             gaze_service, idle_service,
│                   #             expression_service, synth_service, conductor,
│                   #             vision_preview_service, vision_service,
│                   #             camera_service, ui_overlay_service,
│                   #             agenda_service, schedule_service,
│                   #             circadian_service, attention_service,
│                   #             rhythm_service, sound_analysis_service,
│                   #             time_service, wake_service,
│                   #             vad_semantic_service
├── behavior/       # Layer 6: behavior_engine, state_machine, emotion_model,
│                   #           boredom_service, voice_controller
└── persona/        # Layer 7: persona_service, long_term_memory
```

---

## Convenções de Código

- Prefixo `nb_` para **tipos, eventos e macros globais** (ex: `nb_event_t`, `NB_EVT_*`, `NB_STATE_*`). Funções de serviço/componente seguem `<servico>_<operação>` sem prefixo obrigatório (ex: `conductor_play`, `audio_service_init`). Decisão registrada em F28 (2026-06-11).
- Arquivos de header com include guard `#ifndef NB_<MODULO>_H`.
- Erros retornam `esp_err_t`. Usar `ESP_ERROR_CHECK` apenas em init (não em runtime).
- Tasks: nome descritivo (`"nb_render_task"`), stack e prioridade documentados no header.
- Constantes de hardware (GPIO, limites de servo) em `nb_hal/nb_hw_config.h` — nunca hardcoded em lógica.

---

## Hardware Ativo (fase inicial)

| Periférico        | Interface   | Status     |
| ----------------- | ----------- | ---------- |
| ESP32-S3 N16R8    | —           | Ativo      |
| ST7789 2" display | SPI2        | Ativo      |
| WS2812 × 2        | RMT         | Ativo      |
| INMP441 mic       | I2S0 (RX)   | Ativo      |
| MAX98357A speaker | I2S1 (TX)   | Ativo      |
| SCS0009 × 2       | UART/FE-TTL | Ativo      |
| Touch (cobre)     | Touch GPIO  | Ativo      |
| microSD           | SPI3        | Ativo      |
| OV2640 câmera     | DVP         | **Adiado** (hardware); visão via server/bridge: **Feito** |
| MPU-6050 IMU      | I2C0        | **Adiado** |
| LiPo + circuito   | —           | **Adiado** |

**Pinos DVP da câmera estão fisicamente conectados na placa. Nunca realocar esses GPIOs.**
Ver `docs/HARDWARE.md` para o mapa completo de pinos.

---

## Server (Python) — `server/`

Esta seção cobre `server/` (companion app Python/aiohttp). As regras de firmware acima
(C17, ESP-IDF, layers, motion safety, etc.) **não se aplicam** a este diretório.
Este é o documento de autoridade também para o server (ver SF-12 em
`docs/ANALISE_SERVER_FINDINGS_2026-06-11.md`); `AGENTS.md` aponta para cá.

### Layout

- O pacote real é `server/noisebot_server/` (`api/`, `internal/`, `tests/` em
  `server/tests/`). Os diretórios `server/api/`, `server/internal/`,
  `server/manifest/`, `server/resource/` na raiz de `server/` são scaffold
  vazio (apenas `.gitkeep`/README) e **não devem ser usados nem referenciados**.
- Piso de Python: `>=3.10` (declarado em `pyproject.toml`). Não usar
  `asyncio.timeout` (3.11+) — usar `asyncio.wait_for`.
- Comando de teste: `cd server && pip install -e .[dev] && pytest`.
- CI: `.github/workflows/server-tests.yml` roda `pytest` em Python 3.10 e 3.11
  a cada push/PR que toque `server/` (SF-10).

### Regras de I/O assíncrono

- **Nunca** I/O bloqueante (rede, `urlopen`, `subprocess` síncrono) direto no
  event loop. Envolver em `await asyncio.to_thread(...)`.
- `LocalIntentProvider.match()` pode acionar handlers (weather, vision) que
  fazem rede — o orchestrator chama `match()` via `asyncio.to_thread`.
- Para HTTP de saída, preferir helper único (sync/async) em vez de `urlopen`
  espalhado pelos módulos.
- Subprocessos: usar `asyncio.create_subprocess_exec` (nunca `shell=True`).

### Secrets, tokens e rede

- Secrets (API keys de LLM etc.) só via variáveis de ambiente, lidas no ponto
  de uso. `config.py` nunca expõe valores — só booleans `*_configured`.
- Endpoints de ops: GET sem token por padrão (bind `127.0.0.1` + allowlist),
  POST sempre exige `X-NB-Token` (comparação timing-safe, `secrets.token_hex`).
- Offline-first: intents locais PT-BR respondem sem LLM; circuit breaker
  degrada graciosamente quando o provider está fora.

### Busca web (`internal/agent/web_search.py`)

Tool `web_search` roda **só no server** (firmware nunca faz HTTPS). Provider
único: Tavily. Devolve contexto limpo para a 2ª passada da LLM — nunca abre as
URLs (leitura profunda fica para uma futura `page_reader.py`, que exigirá
proteção SSRF real). Resultados são **dados externos não confiáveis**: a LLM não
obedece instruções contidos em títulos/trechos.

Ajuste fino por env (todas opcionais; defaults entre colchetes):

| Variável                       | Default      | Efeito                                                          |
| ------------------------------ | ------------ | -------------------------------------------------------------- |
| `TAVILY_API_KEY`               | —            | Sem a chave a tool retorna erro (degrada gracioso).            |
| `NOISEBOT_SEARCH_DEPTH`        | `advanced`   | `advanced` (precisão máx., ~2x créditos) ou `basic`.          |
| `NOISEBOT_SEARCH_MIN_SCORE`    | `0.30`       | Corta hits abaixo do score (0–1). Se zeraria tudo, mantém o melhor. |
| `NOISEBOT_SEARCH_NEWS_DAYS`    | `7`          | Janela de recência (dias) no modo `news`.                      |
| `NOISEBOT_SEARCH_CACHE_TTL_S`  | `300`        | TTL do cache em memória; modo `news` é limitado a 120s.        |
| `NOISEBOT_SEARCH_LANG`         | `pt-BR`      | Idioma da busca.                                               |
| `NOISEBOT_SEARCH_REGION`       | `BR`         | Boost de localização (→ `country` do Tavily, só topic general).|
| `NOISEBOT_SEARCH_PROVIDER`     | `tavily`     | Provider (hoje só `tavily`).                                   |

- Precisão: depth `advanced` + filtro por score + ordenação por relevância +
  `days` em news. Cache em memória tem teto de 256 entradas (sem vazamento).
- **Separação dashboard vs. robô:** as `sources` (com `snippet`/`score`/
  `published`) e o bloco de métricas `_debug["search"]` são **só para o
  dashboard** — vão ao event bus, nunca ao firmware (`output.py` só envia o
  texto falado ao adapter). O robô responde com 2-3 frases citando o dado
  exato; se os resultados não respondem, diz isso em vez de ser vago.

### Interações multimodais do dashboard

`POST /api/interactions` é o canal autenticado do agente no dashboard. Aceita
texto e um anexo: imagem JPEG/PNG/WebP de até 5 MB ou documento PDF/DOCX/TXT de
até 10 MB. O anexo é validado pelo conteúdo, mantido apenas em memória e
processado no server; bytes, caminhos e conteúdo bruto nunca atravessam o bridge
para o firmware. Os 12 anexos mais recentes ficam disponíveis ao chat por até
30 minutos em cache de memória, via GET autenticado; nunca são persistidos em
disco.

- `response_mode=dashboard`: resposta detalhada somente na interface; não envia
  expressão, texto, TTS nem eventos de sessão ao firmware e não ocupa a FSM do
  turno de voz, permitindo que o robô continue conversando em paralelo.
- `response_mode=robot`: mantém o fluxo normal de resposta do robô.
- O dashboard mostra prévia antes do envio, exibe a imagem na mensagem e abre
  uma visualização ampliada ao clicar.
- Documentos aparecem no histórico e podem ser reabertos enquanto estiverem no
  cache. PDF gera citações por página, DOCX por parágrafo e TXT por intervalo de
  linhas. PDF escaneado sem camada textual ainda não recebe OCR.
- Pesquisa web também pode ser usada no modo dashboard sem acionar o firmware.
- Contexto extraído de anexos é dado externo não confiável e não pode fornecer
  instruções ao agente.
- Áudio deve reutilizar esse endpoint e a mesma fronteira de isolamento quando
  for implementado.

---

## Documentação de Referência

| Arquivo                | Conteúdo                                        |
| ---------------------- | ----------------------------------------------- |
| `docs/PROJECT.md`      | Visão geral, objetivos, princípios de produto   |
| `docs/ARCHITECTURE.md` | Camadas, componentes, event bus, tasks, memória |
| `docs/ROADMAP.md`      | Blocos e etapas detalhadas com critérios        |
| `docs/HARDWARE.md`     | Pinos, barramentos, restrições de GPIO          |
| `docs/PERSISTENCE.md`  | NVS vs SD, estrutura de diretórios, políticas   |
| `docs/ENERGY.md`       | Orçamento de energia, barramento 5V, brownout   |
| `docs/SERVO_SAFETY.md` | Parâmetros de safety, protocolo de liberação    |
