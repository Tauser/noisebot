# NoiseBot - Auditoria de Limpeza e Roadmap

Status: rascunho inicial, sem remocoes aplicadas.

Objetivo: reduzir ruido do repositorio sem perder historico util. A limpeza deve
priorizar evidencias do proprio projeto, manter rollback simples e separar
documentacao historica de documentacao operacional.

## Decisao Atual

- Nao atualizar o Knowledge OS externo/Obsidian nesta rodada.
- Nao remover codigo de firmware sem build verde e referencia cruzada.
- Nao apagar docs historicos de voz antes de consolidar o estado atual em um
  documento menor.
- Roadmap principal mostra apenas o que importa agora; historico fechado sai do
  fluxo principal e fica em arquivos de referencia/arquivo.
- Etapa 16.2 (`tts_service` HTTP local no firmware) removida do roadmap ativo:
  o TTS de produto permanece no fluxo server/bridge atual, e manter um servico
  HTTP paralelo no firmware duplicaria responsabilidade sem ganho imediato.

## Diagnostico Inicial

O problema principal encontrado ate agora e documentacao acumulada:

- `docs/ROADMAP.md` antigo tinha aproximadamente 198 KB e misturava plano vivo,
  historico fechado, backlog antigo e decisoes ja superadas. Uma copia local
  pode ficar em `docs/archive/`, mas essa pasta nao precisa ir para o Git. O
  novo `docs/ROADMAP.md` e um painel ativo de decisao.
- A area de Voice Audio v2 tem multiplos roadmaps paralelos:
  - `docs/VOICE_PIPELINE.md`
  - `docs/VOICE_AUDIO_V2_ARCHITECTURE.md`
  - `docs/VOICE_AUDIO_V2_NEXT_PHASES.md`
  - `docs/VOICE_AUDIO_V2_RELEASE_CHECKLIST.md`
  - `docs/OBSIDIAN_VOICE_AUDIO_V2_KNOWLEDGE.md`
- O roadmap ainda mostra itens antigos como abertos mesmo quando etapas
  posteriores registram fechamento funcional em hardware.
- Alguns blocos futuros, como wake word customizada e TTS HTTP no firmware,
  continuam no roadmap mas nao sao prioridade atual por decisao de produto.

## Feitos Ja Consolidados Dos Roadmaps Paralelos

Esta secao centraliza o que os roadmaps paralelos ja registram como feito, para
nao voltar como pendencia no roadmap vivo.

### Voice Audio v2

Origem: `docs/VOICE_AUDIO_V2_ARCHITECTURE.md`,
`docs/VOICE_AUDIO_V2_NEXT_PHASES.md`,
`docs/VOICE_AUDIO_V2_RELEASE_CHECKLIST.md` e `docs/VOICE_PIPELINE.md`.

- Voice Audio v2 esta fechado funcionalmente em hardware.
- Opus v2 foi promovido a capability oficial/default local do server, com
  fallback PCM16 preservado.
- Capture v2 esta fechado como TX owner controlado.
- Activity v2 foi validado como decisor dentro de sessoes reais.
- Audio IO v2 esta fechado para RX distribuido, TX observado e telemetria.
- Playback v2 esta fechado para contrato de speaker/SAY, incluindo ownership
  controlado por gate/janela real.
- Barge-in por wake word, no-echo e cancelamento de fala foram validados.
- `voice-release-check` virou gate operacional agregado e foi endurecido contra
  falhas HTTP de firmware e `/ai/metrics`, sem traceback opaco.
- Backlog tecnico P de reducao do `audio_service.c` foi fechado
  pragmaticamente; `audio_service.c` permanece como ponte/compatibilidade onde
  ainda for necessario.
- AEC device-side permanece em standby/bloqueado no hardware atual por falta de
  referencia limpa de eco; nao deve ser tratado como pendencia do release atual.

Implicacao no roadmap vivo:

- Blocos antigos de Voice Audio v2, fases I a P, checklists parciais, A/Bs e
  migracoes internas devem virar historico/arquivo, nao backlog ativo.
- O roadmap ativo deve manter apenas gates operacionais de regressao de voice,
  nao reabrir a implementacao do Voice Audio v2.

### Bridge, Server e Produto de Voz

Origem: `docs/BRIDGE_V2.md`, `docs/VOICE_PIPELINE.md` e `docs/ROADMAP.md`.

- Bridge/server ja cobre STT, LLM, TTS, intents locais, device commands,
  feedback visual de sessao, metricas e harness de regressao.
- Conversation Protocol v2 esta validado em firmware/server, com eventos de
  sessao e falhas terminais nomeadas.
- Robot Tools v2, overlays expressivos de bridge e setup/diagnostico de produto
  aparecem como concluidos no roadmap principal.
- Auditoria de imports/testes concluida (2026-06): `noisebot_server` nao tem
  nenhuma dependencia de runtime em `bridge`/`bridge_v2` (`grep` por
  `import bridgev2|from bridgev2` no pacote nao retorna nada; o entrypoint real
  e `python -m noisebot_server`). Os ~28 testes de paridade que importavam
  `bridgev2.*` em `test_server_facade.py` foram reescritos para serem
  autocontidos. `bridge/` e `bridge_v2/` ja podem ser tratados como candidatos
  a remocao, nao apenas consolidacao — falta so o checklist final (ver
  "Candidatos a Auditoria de Codigo").

Implicacao no roadmap vivo:

- Etapas 12.13, 12.16 e 12.19 devem ser reconciliadas: criterios antigos ainda
  abertos precisam ser marcados como historico superado ou convertidos em gates
  operacionais atuais.

### Camera e Visao

Origem: `docs/CAMERA_INTEGRATION.md` e `docs/ROADMAP.md`.

- Camera OV2640 esta integrada para captura sob demanda e observacao basica.
- Testes de soak de camera registraram operacao longa com frames validos,
  camera fechada ao final e Voice Audio v2 release-check verde.
- Observacao visual basica e caminho de visao via bridge/server ja existem.
- Politica low-res foi adotada para runtime de visao, inspirada pela comparacao
  StackChan/Xiaozhi, para reduzir pressao no firmware.
- Captura segura foi alinhada ao modelo StackChan: sessao explicita,
  fechamento previsivel e observacao sem JPEG quando possivel.
- Render/FPS foi protegido durante camera: houve recuperacao de FPS,
  reducao de dirty rect e pausa de tilt durante sessao de camera.
- Overlay discreto de camera ativa foi implementado no firmware, compondo com
  o overlay de microfone.
- Icones de overlay migraram para assets PBM gerados em C; a tentativa de SVG
  foi revertida.
- `vision-soak` e `vision-presence-trial` existem no server para validar soak,
  ausencia, presenca, perda, FPS e falsos positivos.
- `vision_service` ja expoe polling diagnostico com start/status/stop para
  observar captura repetida sem acoplar comportamento autonomo.
- Presenca real ainda nao esta fechada: eventos `PRESENCE_DETECTED` e
  `PRESENCE_LOST`, falsos positivos e FPS com `vision_service` ativo continuam
  sendo o trabalho vivo.

Implicacao no roadmap vivo:

- 13.0 deve ser tratado como parcial/feito operacionalmente.
- 13.1 continua como proxima etapa real.
- 13.2 face tracking, 13.3 gestos e 18.2 camera stream externo ficam no proximo
  estagio/backlog, nao no foco imediato.

### Itens Feitos Ou Parcialmente Feitos Que Precisam Entrar No Roadmap Limpo

Origem: historico Git recente, testes e varredura de codigo local.

Confirmados por commits/testes/docs:

- `ui_overlay_service`: overlay de microfone, camera ativa, toasts de sessao,
  fonte Montserrat/StackChan e assets PBM gerados.
- `touch_service`: sensibilidade default mais conservadora para cobre,
  sensibilidade persistida no boot, aplicacao runtime e stack interna.
- `voice-release-check`: gates de Voice v2, ownership, codec/playback/capture,
  falha HTTP do firmware, falha em `/ai/metrics` e CLI sem traceback.
- `audio_service` / Voice v2: reducao de acoplamento RX, isolamento de caminhos
  legados de PLAY_STOP, SAY, playback local e fallback TX.
- `audio_playback_service_v2`: fila SAY, pacing do server, lifecycle de fala,
  ownership controlado e telemetria de speaker/HAL.
- `server` debug/ops: `vision-soak`, `vision-presence-trial`,
  `transcript-live`, `voice-v2`, `capture-v2`, `playback-v2` e `codec-v2`.
- Agenda/timers no server: app state, comandos locais, importacao de agenda do
  firmware e testes para timer, alarm, reminder e repeat mask existem, mas o
  fluxo de produto ainda precisa ser fechado ponta a ponta.

Existem no codigo e precisam de verificacao antes de marcar como concluidos:

- `time_service`: SNTP/fuso horario via `NB_EVT_WIFI_IP_ACQUIRED`.
- `agenda_service`: precisa validar criacao/listagem/cancelamento/disparo real,
  persistencia e feedback visual/sonoro no firmware.
- `boredom_service`: escalada criativa de ociosidade e pausa por estado.
- `bridge_v2/` vs `server/`: concluido — `server/` absorveu a implementacao,
  roda de forma autocontida, e `bridge/`/`bridge_v2/` ja foram removidos do
  repositorio apos o checklist final (ver "Candidatos a Auditoria de Codigo").

### Decisoes De Produto Ja Tomadas Nesta Limpeza

- Servos/motion safety ficam fora do foco imediato porque os servos ainda nao
  estao conectados.
- Wake word customizada nao sera feita agora.
- `tts_service` HTTP local no firmware foi removido do roadmap ativo por
  duplicar o TTS via server/bridge.
- Status rail invisivel para icones persistentes foi adicionado como nova etapa
  ativa de overlay: centraliza mic, camera, speaker, bridge, WiFi e alertas no
  `ui_overlay_service`, sem permitir desenho direto por servicos.

## Prioridade Recomendada

1. Consolidar documentacao e roadmap.
2. Auditar codigo morto e diretorios legados.
3. Remover ou arquivar apenas o que tiver evidencia clara de nao uso.
4. Rodar testes/build depois de cada remocao real.

## Estrutura Proposta

Manter documentacao operacional curta:

- `docs/PROJECT.md`: visao de produto e estado atual.
- `docs/ARCHITECTURE.md`: arquitetura viva do firmware/server/bridge.
- `docs/ROADMAP.md`: somente roadmap ativo e proximo estagio.
- `docs/CAMERA_INTEGRATION.md`: plano vivo de camera/visao.
- `docs/BRIDGE_V2.md`: referencia consolidada do bridge/server.
- `docs/VOICE_AUDIO_V2_ARCHITECTURE.md`: contrato atual de voice audio v2,
  reduzido para estado final e invariantes.

Mover para historico/arquivo depois de consolidar:

- relatorios A/B de voz antigos;
- checklists de fases ja fechadas;
- documentos de proximas fases que hoje sao historico;
- material preparado especificamente para Obsidian.

Movido localmente para `docs/history/` nesta rodada:

- relatorios A/B e qualidade de voz;
- `VOICE_PIPELINE.md`;
- `VOICE_AUDIO_V2_NEXT_PHASES.md`;
- `VOICE_AUDIO_V2_RELEASE_CHECKLIST.md`;
- material Obsidian;
- referencias curtas antigas de StackChan/Xiaozhi;
- `BRIDGE_V2_TTS_LOCAL.md`.

## Roadmap Vivo Sugerido

### Agora

- Vision/presence 13.1: detector real de presenca, eventos `PRESENCE_DETECTED`
  e `PRESENCE_LOST`, teste de falsos positivos e FPS.
- Agenda local 14.1: fechar timers, lembretes e alarmes como produto real
  ponta a ponta, sem depender de LLM e sem prometer criacao incompleta.
- Camera/voice/render soak: validar camera, bridge, TTS e display juntos em
  teste longo.
- Docs cleanup: centralizar estado atual e remover duplicidade do roadmap.

### Proximo Estagio

- Motion Safety e servos quando os servos estiverem conectados.
- Face tracking e gestos somente depois de presenca confiavel.
- I2C/sensores somente quando o hardware correspondente entrar no bring-up.
- TTS HTTP no firmware foi removido do roadmap ativo por decisao de produto.
- Wake word customizada fica explicitamente adiada por decisao de produto.
- Status rail invisivel entra como melhoria de produto/operacao antes de novos
  overlays decorativos, porque reduz conflito visual e padroniza indicadores.

## Candidatos a Auditoria de Codigo

Estes itens precisam de verificacao antes de qualquer remocao:

- `bridge/` e `bridge_v2/`: **removidos do repositorio (2026-06-08)** apos
  checklist final concluido —
  (1) scripts de servico: sem lacuna, `server/internal/service/manager.py` ja
  cobre Windows Task Scheduler e systemd com mais cobertura que os scripts
  shell do bridge_v2; (2) `debug/{record,replay}.py` do bridge_v2 eram stubs
  vazios (so TODO), nada a portar — `fake_firmware.py`/`manual.py` do server
  cobrem o equivalente; (3) `metrics/{registry,timeline}.py` ja tem
  equivalente (`MetricsRegistry` em `agent/metrics.py`,
  `SessionContext.timeline`/`mark()`); so `metrics/logfmt.py` (log JSON
  estruturado) ficou sem par — server usa `logging` `%`-format, risco baixo;
  (4) spot-check de `bridge/tests/` vs `server/tests/`: unico arquivo notavel
  foi `test_voice_session.py`, mas ele testava a API `noisebot_bridge`
  (`VoiceSessionRuntime`/`classify_session_outcome`) — uma geracao mais antiga
  que o bridge_v2, ja sem equivalente no server. Os comportamentos que cobria
  (rejeicao por baixa confianca, fallback de LLM, device intents, barge-in)
  ja tem cobertura propria no server com a arquitetura atual (Orchestrator
  assincrono + FSM). `server/README.md` ja foi corrigido para nao descrever
  mais delegacao ao `bridge_v2`.
- `main/servo_test.*`: verificar se ainda e usado no build atual.
- docs e scripts de fases antigas de voz: devem virar arquivo historico ou
  ser removidos se o conteudo ja estiver consolidado.
- diretorios/vendor de `components/LovyanGFX`: nao tratar como codigo morto
  sem confirmar impacto no componente vendorizado.

## Checklist de Execucao

- [x] Gerar mapa de docs atuais e dependencias cruzadas.
- [x] Consolidar feitos principais dos roadmaps paralelos nesta auditoria.
- [x] Criar roadmap ativo reduzido.
- [x] Criar arquivo historico local para fases fechadas.
- [x] Atualizar `README.md` e indice de docs.
- [x] Criar `docs/README.md` como indice de docs vivos, referencias e historicos.
- [x] Auditar CMake e imports Python para detectar diretorios realmente usados.
- [x] Remover/arquivar em commits pequenos (`bridge/` e `bridge_v2/` removidos
      em 2026-06-08, sem dependencia de runtime confirmada).
- [x] Rodar testes de server/bridge conforme area tocada.
- [ ] Rodar `idf.py build` apos qualquer remocao que afete firmware.
