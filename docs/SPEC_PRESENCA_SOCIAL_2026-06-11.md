# SPEC — Presença Social Anônima (v2.1)

**Data:** 2026-06-11 · **Status:** pronta para implementação
**Substitui:** spec v1 (presença social sobre VisionPipeline). Mudanças da v2: fusão de sinal offline-first, FSM social única no firmware, zero mudança de protocolo bridge, duty cycle de câmera, rate-limit de ping, wake-on-presence opt-in.
**v2.1 (revisão pós-leitura do código):** eventos renomeados para `NB_EVT_SOCIAL_PRESENCE_*` (os `NB_EVT_PRESENCE_DETECTED/LOST` existentes em `nb_events.h` ficam como sinal bruto da visão — vocabulários separados); serviço proibido de tocar câmera (consome só eventos); campos de implementação da regra de desconexão; rate-limit decisório movido para o behavior_engine.
**Pré-requisitos já entregues:** YuNet funcional no servidor, VisionPipeline (IDLE/ACQUIRE/TRACK/LOST), MSG_GAZE/MSG_FACE_BOX no bridge, card de câmera no dashboard.

---

## 0. Regras de contorno (inegociáveis)

Ler AGENTS.md e CLAUDE.md antes. Além delas:

- **Não** implementar reconhecimento facial. Detecção = "tem alguém"; reconhecimento = "sei quem é" (futuro, fora desta spec).
- **Não** mudar resolução da câmera (240×240 YUV fica).
- **Não** tocar servo/motion (motion_safety não está verde).
- **Não** alterar o protocolo bridge (nenhum MSG novo — ver §3).
- **Não** contaminar warmth/trust/curiosity do persona_service nesta etapa.
- Ausência **nunca** gera tristeza. Ausência é estado normal.
- Offline-first: toda a camada social precisa funcionar (degradada) sem servidor.
- Commits pequenos e rollback-friendly; cada fase (§7) é um commit funcional.

---

## 1. Conceito

Detecção de face vira **comportamento social perceptível com continuidade**, não gaze tracking:

- perceber que alguém chegou → **attention ping** (anúncio curto, não-verbal);
- manter atenção enquanto há alguém → gaze suave (manutenção, não anúncio);
- presença longa → **companhia silenciosa** (idle calmo, olhares ocasionais);
- ausência → descompressão gradual até idle calmo/sleep, sem drama;
- tudo isso **sem saber quem é a pessoa** (presença anônima).

Diferencial sobre o StackChan: ele tem charme visual e modifiers temporários; o NoiseBot adiciona **continuidade interna** — presença, companhia, disponibilidade, rotina anônima.

---

## 2. Arquitetura — uma FSM social, dois sensores

### 2.1 Fusão de sinal (correção offline-first da v1)

A presença social consome um **sinal fundido de duas fontes**, nunca apenas o servidor:

| Fonte | O que dá | Disponibilidade |
|---|---|---|
| `vision_service` (firmware, heurística motion+luma+contraste, `presence_score`) | presença **provável**, sem posição | sempre (offline) |
| VisionPipeline via `MSG_FACE_BOX` (servidor, YuNet) | presença **confirmada** + posição do rosto | só com servidor conectado |

Semântica da fusão:
- face box válido ⇒ CONFIRMED (fonte mais forte vence);
- só heurística alta ⇒ presença provável (sustenta companhia/hold, **não** dispara attention ping de "retorno" — evita ping falso);
- servidor desconecta durante presença confirmada ⇒ **degrada** para provável, sem evento de saída e sem ping quando reconectar com a mesma presença contínua.

Implementação da regra de desconexão: o serviço guarda `last_confirmed_source` (`face_box` | `heuristic`) e `probable_since_ms`. Ao receber `NB_EVT_BRIDGE_DISCONNECTED` em PRESENT/ENGAGED: **não** transiciona para LEFT_RECENTLY; rebaixa a confiança para "provável" e passa a sustentar o estado pela heurística local. Na reconexão com a mesma presença contínua, o primeiro FACE_BOX válido apenas restaura a confiança — não é ACQUIRED nem RETURNED.

### 2.2 Onde mora a FSM: `presence_semantic_service` (firmware, novo componente)

Precedente arquitetural do próprio projeto: `touch_service` → `touch_semantic_service`. Mesmo padrão:

- **VisionPipeline (servidor) continua sensor** — dono do debounce de *detecção* (2 hits para confirmar, 3 misses para perder). Nenhuma lógica social nele.
- **`presence_semantic_service` (Layer 5, `components/services/`)** — única dona da FSM *social* e dos timers de ausência. **Consome apenas eventos — nunca toca câmera nem chama camera_service/vision_service para capturar.** Um dono captura (vision_service/camera), outro interpreta (este serviço). Subscribes: `NB_EVT_BRIDGE_FACE_BOX`, `NB_EVT_PRESENCE_DETECTED`, `NB_EVT_PRESENCE_LOST` (sinal bruto legado do vision_service — permanece com a semântica atual), `NB_EVT_BRIDGE_CONNECTED`, `NB_EVT_BRIDGE_DISCONNECTED`. Publica os eventos sociais no bus.
- **Proibido**: terceira máquina de estados de presença em qualquer outro lugar; behavior/conductor consomem eventos, não estado bruto de face.

### 2.3 FSM social (estados internos do serviço)

```
NO_ONE ──score alto/box──▶ MAYBE_SOMEONE ──confirmação──▶ PRESENT ──▶ ENGAGED
   ▲                            │(timeout sem confirmar)      │  (companhia
   │                            ▼                             │   silenciosa
   └──── ALONE_SETTLED ◀── AWAY ◀── LEFT_RECENTLY ◀───────────┘   após N min)
```

| Estado | Significado | Entrada |
|---|---|---|
| `NO_ONE` | sem presença | boot; fim de AWAY longo |
| `MAYBE_SOMEONE` | sinal inicial não confirmado | score alto OU primeiro box |
| `PRESENT` | presença confirmada | box estável (ou heurística sustentada ≥10 s se offline) |
| `ENGAGED` | companhia silenciosa | PRESENT contínuo ≥ N min (default 3) |
| `LEFT_RECENTLY` | sumiu há pouco; segurar contexto | perda de box+score |
| `AWAY` | ausência média | LEFT_RECENTLY ≥ 30 s |
| `ALONE_SETTLED` | ausência longa | AWAY ≥ 120 s |

Timers de descompressão (defaults; todos em config NVS, não hardcoded):
- 0–5 s sem sinal: **hold** — manter último gaze/estado, não limpar nada;
- 5–30 s: olhar de busca ocasional, expressão levemente curiosa (sem tristeza);
- 30–120 s: retorno ao idle calmo (baseline IDLE manda);
- 120 s+: sinalizar à state_machine (ver §4.3) — SLEEPING/SILENT_COMPANY decidem lá.

### 2.4 Eventos semânticos (novos tipos no `nb_events.h`)

Namespace `SOCIAL_PRESENCE` — deliberadamente distinto dos `NB_EVT_PRESENCE_DETECTED/LOST` existentes (sinal bruto da visão, que permanecem intocados):

```
NB_EVT_SOCIAL_PRESENCE_ACQUIRED      NO_ONE/ALONE→MAYBE_SOMEONE
NB_EVT_SOCIAL_PRESENCE_CONFIRMED     →PRESENT (primeira vez da sessão)
NB_EVT_SOCIAL_PRESENCE_ENGAGED       →ENGAGED (companhia silenciosa)
NB_EVT_SOCIAL_PRESENCE_LOST_SHORT    →LEFT_RECENTLY
NB_EVT_SOCIAL_PRESENCE_AWAY          →AWAY
NB_EVT_SOCIAL_PRESENCE_SETTLED       →ALONE_SETTLED
NB_EVT_SOCIAL_PRESENCE_RETURNED      AWAY/ALONE→PRESENT (retorno após ausência ≥ 60 s)
```

Payload: estado anterior + novo (mesmo encoding u32 de `NB_EVT_STATE_CHANGED`). Consumidores: behavior_engine, conductor (via behavior), expression/led (via behavior), state_machine. **HAL nunca.**

**Histerese semântica é responsabilidade do serviço:** os eventos sociais não podem flapar — RETURNED só após ausência ≥ 60 s, CONFIRMED exige estabilidade, perda momentânea (<5 s) não gera evento nenhum. Quem consome os eventos pode confiar neles sem re-filtrar.

---

## 3. Protocolo bridge: NENHUMA mudança

O firmware já recebe tudo que precisa: `MSG_FACE_BOX` com box vazio (w=0) significando "sem rosto" e `MSG_GAZE` para o alvo. O `presence_semantic_service` deriva confirmação/perda do stream de FACE_BOX existente. O servidor **não** envia eventos de presença — quem decide semântica social é o firmware (offline-first de verdade).

No servidor, mudanças mínimas e locais:
- VisionPipeline: nenhum estado novo; apenas expor contadores (§6).
- `app_state`: campos `presence_now`, `presence_state` (espelho lido do firmware via diag/status, não fonte), `companionship_today_s`, `companionship_recent_score`, `interaction_readiness` — derivados, zero persistência por tick.
- Contexto LLM (payload_builder): quando presente — "O usuário parece estar fisicamente presente diante do robô."; em companhia — "...em companhia silenciosa há alguns minutos."; ausente — "Não há presença visual confirmada agora." Discreto: **nunca** gerar fala espontânea por presença.

---

## 4. Comportamento visual

### 4.1 Attention ping (o anúncio)

Nova ação do conductor: `NB_ACTION_NOTICE`, 2–3 variações **sem áudio** (ex.: A: blink + olhos abrem um pouco + ATTENTIVE 1,5 s; B: micro-gaze para a pessoa + CURIOUS 1,2 s; C: blink duplo + leve brilho de LED). Ganha de graça: anti-repeat, interrupção limpa, respeito ao baseline IDLE (ping é transitório/overlay, nunca substitui o baseline).

Gatilhos (regra no behavior_engine, padrão tabela existente):
- `SOCIAL_PRESENCE_CONFIRMED` (primeira da sessão) → ping;
- `SOCIAL_PRESENCE_RETURNED` (ausência ≥ 60 s) → ping;
- nada mais dispara ping. Em especial: reconexão de servidor com presença contínua **não** dispara.

**Divisão de responsabilidade em dois níveis:** o `presence_semantic_service` garante que os *eventos* não flapam (histerese semântica, §2.4); o **behavior_engine decide agir ou suprimir** — guards de estado (não pingar em RESPONDING, SLEEPING exceto §4.4, MEDITATION, conductor ocupado) **e o rate-limit de máx. 4 pings/hora vivem no behavior_engine**, que é quem sabe se vai agir. O `ping_count`/`ping_suppressed_count` são contabilizados no behavior e expostos via diagnostics.

### 4.2 Tracking (a manutenção)

Durante PRESENT/ENGAGED: gaze suave via `MSG_GAZE` (EMA + deadzone já implementados no pipeline do servidor); o gaze **não compete** com o idle — idle_service continua dono dos motifs; o gaze de presença entra como bias do gaze_service, não como override.

Em ENGAGED (companhia silenciosa):
- idle mais calmo: reduzir frequência de motifs aleatórios (idle_service ganha um modificador "calm", consumido via evento — não chamada direta de quem não pode);
- olhares ocasionais de volta para a pessoa (motif novo, só em NEUTRAL, seguindo os guards existentes);
- **duty cycle de câmera**: presença sustentada pela heurística local; face check do servidor cai para ~1 a cada 30 s (o VisionPipeline ganha modo `HOLD` de cadência reduzida, comandado pelo... servidor mesmo, ao observar TRACK estável — sem protocolo novo). Economiza o recurso mais disputado no estado mais longo.

### 4.3 Ausência

`presence_semantic_service` **não** comanda sleep. Ele publica os eventos; a state_machine existente consome `PRESENCE_AWAY`/`PRESENCE_SETTLED` como **modulador do timer próprio** de idle→SLEEPING (ex.: SETTLED encurta o timeout; PRESENT/ENGAGED o congela). Um dono por decisão: sleep é da state_machine, presença é do semantic service.

### 4.4 Wake-on-presence (opt-in, default OFF)

Config NVS `presence_wake_en` (default 0). Quando ON: SLEEPING + heurística local alta ≥ 5 s → `PRESENCE_ACQUIRED` pode acordar via fluxo de wake existente. Default OFF porque "ele me viu chegar" pode ser charmoso ou creepy — decisão do usuário, não do firmware.

Implementação atual: a opção é exposta como `presence_wake_enabled` em
`/api/config` e `/api/config/all`, e aparece no bloco `social_presence` de
`/api/diag`. O wake dispara em `SLEEPING` quando há
`SOCIAL_PRESENCE_CONFIRMED` (face ou heurística sustentada) ou `RETURNED` com
face confirmada; `RETURNED` por heurística sozinha é suprimido para evitar
acordar com um único falso positivo após ausência longa.

---

## 5. Persona — fora desta etapa

- warmth/trust/energy/curiosity: **intocados**.
- Presença é modulador temporário/contextual. Vínculo permanente por presença recorrente é etapa futura (e exigirá decisão explícita de privacidade).
- Único acumulador novo: `companionship_today_s` (servidor, app_state, reset diário; persistência no máximo 1×/h via persistence_task — nunca por tick).

---

## 6. Telemetria e observabilidade

Firmware:
- `presence_semantic_service`: estado atual, `presence_session_s`, `absence_s`, `last_seen_at`, `confidence` (confirmada/provável), contadores de transição (`presence_confirmed_count`, `presence_returned_count`, `presence_away_count`);
- behavior_engine: `ping_count`, `ping_suppressed_count` (guards + rate-limit);
- tudo exposto no endpoint de diag existente e no status rail (ícone de presença é candidato natural ao rail 16.2).

Servidor (VisionPipeline status + dashboard):
- contadores por estado do pipeline + cadência atual (normal/HOLD);
- card do dashboard mostra estado social lido do firmware (fonte da verdade é o firmware).

Um `log.info`/`NB_LOGI` por transição social; ticks em debug.

---

## 7. Fases de implementação (cada uma = commit funcional + rollback trivial)

1. **F1 — Eventos + serviço esqueleto:** `nb_events.h` (+7 tipos `NB_EVT_SOCIAL_PRESENCE_*`), componente novo `presence_semantic_service` com subscribes em `NB_EVT_BRIDGE_FACE_BOX`, `NB_EVT_PRESENCE_DETECTED`, `NB_EVT_PRESENCE_LOST`, `NB_EVT_BRIDGE_CONNECTED/DISCONNECTED`; FSM completa + contadores; **nenhum ping, nenhuma expressão, só logs de transição**. *Valida: transições corretas no log com pessoa real entrando/saindo, incluindo o cenário de desconexão do bridge durante PRESENT.*
2. **F2 — Attention ping:** `NB_ACTION_NOTICE` no conductor + regras no behavior_engine + rate-limit. *Valida: critérios §8 itens 1–3.*
3. **F3 — Companhia silenciosa:** modificador calm no idle + motif de olhar ocasional + modulação do timer de sleep na state_machine. *Valida: itens 4–5.*
4. **F4 — Telemetria + dashboard + contexto LLM.** *Valida: itens 6–7.*
5. **F5 (opcional) — duty cycle HOLD no pipeline + wake-on-presence opt-in.**

Ordem de inspeção antes da F1 (mantida da v1): vision_pipeline.py, face_loop/transport/adapter (eventos FACE_BOX no firmware), expression/conductor/behavior/idle (ponto de encaixe do ping), persona/long_term_memory (garantir não-contaminação).

---

## 8. Critérios de aceite

1. Face estável → exatamente **um** ping perceptível e curto; tracking suave depois.
2. Detecção flapando (cobrir/descobrir a câmera 10× em 1 min) → **zero** metralhadora de pings (rate-limit + histerese comprovados por `ping_suppressed_count`).
3. Perda momentânea (<5 s) → nenhuma reação visível (hold).
4. Ausência longa → idle calmo → sleep pela state_machine; **nenhuma expressão triste** em todo o fluxo.
5. Presença de 10 min → ENGAGED com idle visivelmente mais calmo e olhares ocasionais.
6. **Teste offline:** desligar o servidor durante PRESENT → degrada para presença provável sem evento de saída; religar com a pessoa ainda lá → **sem** ping de retorno. Robô sem servidor desde o boot → companhia/sleep funcionam só com heurística.
7. warmth/trust idênticos antes/depois de uma sessão de presença (assert em teste).
8. Voice/bridge/render sem regressão (release-check existente verde); camadas e event bus respeitados (behavior não toca HAL; serviço novo não chama camera_hal).
