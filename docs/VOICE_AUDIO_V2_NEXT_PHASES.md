# Voice Audio v2 - Proximas Fases Pos-Opus

Data: 2026-05-31
Status: roadmap operacional pos-fechamento da migracao Opus v2.
Branch de referencia: `voice-reference-architecture`.

Este documento continua `VOICE_AUDIO_V2_ARCHITECTURE.md` depois do fechamento
da migracao Opus v2. A meta agora nao e "colocar mais teste por teste": e
terminar a separacao arquitetural do audio sem quebrar o NoiseBot que ja
conversa bem.

Checklist operacional da Fase M parcial:
`docs/VOICE_AUDIO_V2_RELEASE_CHECKLIST.md`.

## Estado Atual

Concluido:

- Opus v2 esta fechado como default local do server por
  `NOISEBOT_AUDIO_DEFAULT_CODEC=opus-v2`.
- O firmware continua anunciando Opus como capability opt-in e preserva PCM16
  como fallback operacional.
- `audio_codec_service_v2` tem worker live proprio, fila curta, egress Opus,
  transport enable/disable, health no server e rollback limpo.
- Validacoes passaram em hardware: turno real Opus, A/B PCM16 vs Opus,
  barge-live, no-echo-live, soak real com intents locais/LLM e `codec-v2
  health`.
- O corte observado de texto foi separado de audio: TTS/playback completou, e
  o problema era visual no `TEXT_SCROLL`; o server agora pagina o texto com
  limite UTF-8 e largura visual aproximada.

Ainda aberto:

- `audio_service.c` continua sendo o dono real de I/O, playback, VAD, pre-roll,
  capture session, bridge TX e parte do roteamento v2.
- `audio_playback_service_v2`, `voice_activity_service_v2` e
  `voice_capture_session_v2` ainda nao assumiram a responsabilidade principal.
- AEC device-side segue bloqueado no hardware atual por falta de referencia
  limpa de playback.
- Follow-up automatico e barge-in sem wake continuam fora do escopo ate haver
  criterio proprio de AFE/AEC/no-echo.

## Principios de Arquitetura

O que continuar preservando:

- I/O de audio separado de codec e de policy.
- Task dedicada para codec Opus, com filas curtas.
- Processor plugavel entre microfone e codec.
- Estados explicitos de conversa: idle, listening, speaking, abort/cancel.
- Abort/cancel limpo quando wake acontece durante fala.
- Capabilities de audio explicitas no protocolo.

O que nao entra diretamente:

- `AUDIO_INPUT_REFERENCE=true`: o NoiseBot atual usa INMP441 + MAX98357A e nao
  possui canal limpo de referencia do speaker.
- AEC device-side como feature geral. No NoiseBot atual, AEC fica bloqueado ate
  existir referencia real ou desenho server-side com timestamps.
- Input/output 24 kHz como requisito. O caminho atual do NoiseBot e 16 kHz,
  mono, e ja esta alinhado ao STT e ao Opus upstream.
- Troca completa do transporte. O bridge TCP local ja funciona e deve ser
  evoluido de forma incremental.
- C++/STL fora das excecoes permitidas no firmware C17 do NoiseBot.

## Fase I - Playback v2 Como Dono Gradual do Downlink

Objetivo: fazer `audio_playback_service_v2` assumir a fila e a politica de
playback/SAY de forma observavel, mantendo o `audio_service` como ponte segura
ate a troca estar validada.

Entregas:

- Fila curta de playback v2 para SAY/PCM local.
- API interna para enqueue, drain, cancel e status.
- Metrica de chunks aceitos, tocados, descartados e cancelados.
- Cancelamento idempotente em barge-in e `audio_play_stop()`.
- Janela clara para descartar chunks SAY antigos depois de cancel.
- Nenhum acesso direto do v2 ao HAL antes do handoff planejado.

Primeiro incremento implementado:

- `audio_playback_service_v2` agora observa o downlink SAY real do
  `audio_service` sem assumir a fila nem o HAL.
- O status de `/api/audio/playback-v2` expoe `bridge_say_observer`,
  `say_queue_depth`, `say_queue_count`, `say_chunks_received`,
  `say_chunks_played`, `say_chunks_dropped`,
  `say_chunks_dropped_listening`, `say_chunks_cancelled` e
  `say_cancel_count`.
- `audio_service` continua dono da fila `bridge_say_q` e do speaker; o v2
  apenas recebe notas de enqueue/play/drop/cancel/idle.
- Validacao local: contrato bridge focado, `server/tests/test_server_facade.py`,
  `bridge/tests` completo e `idf.py build`.
- Validacao em hardware apos flash: `/api/audio/playback-v2` expos
  `bridge_say_observer=true` e contadores `say_*` durante downlink real. No
  caminho representativo do orquestrador via `/debug/transcript`, o turno
  adicionou 237 chunks recebidos e 237 tocados, sem drops novos, com
  `say_queue_count=0`, `last_error=ESP_OK`, `tts_completed=true`,
  `tts_say_begin_sent=true` e `tts_say_end_sent=true`.
- Observacao de bancada: `/api/profile/test-voice` tambem exercitou SAY real e
  provou o observador, mas gerou drops por nao passar pelo `OutputScheduler` do
  turno conversacional. Para aceite da Fase I, usar o caminho do orquestrador
  como referencia.
- Pos-validacao: `codec-v2 health` permaneceu `healthy=true`, `status=ok`, zero
  drops, fila egress zero e `opus_codec_error=0`; `capture-v2 status` permaneceu
  desligado em `IDLE_SESSION`, com `ESP_OK`.

Segundo incremento implementado:

- Validacao arquitetural confirmou o alvo da Fase I: fila curta de downlink,
  drain/cancel explicitos e output final ainda abaixo do servico de audio. A
  referencia interna para audio continua sendo o pipeline
  `decode queue -> decoder -> playback queue -> speaker`, adaptado ao hardware
  e aos contratos do NoiseBot.
- `audio_playback_service_v2` passou a ser dono da fila estatica de SAY real
  (`audio_playback_service_v2_say_enqueue/dequeue/cancel`), com profundidade
  mantida em 16 chunks para preservar o comportamento ja validado.
- `audio_service` nao possui mais `bridge_say_q`; ele apenas inicia o modo
  `PLAY_BRIDGE_SAY`, drena chunks pelo contrato v2 e continua sendo o unico
  dono do HAL/speaker neste incremento.
- `/api/audio/playback-v2` agora expoe `bridge_say_queue_owner=true` quando a
  fila SAY esta sob o v2.
- Cancelamento de fila SAY pelo contrato v2 e idempotente para nao inflar
  `say_cancel_count` quando o mesmo stop passa por mais de um ponto seguro.
- Validacao local: contrato firmware Voice Audio v2 focado e `idf.py build`.
- Validacao em hardware apos flash: status inicial de `/api/audio/playback-v2`
  confirmou `bridge_say_queue_owner=true`, `say_queue_depth=16`,
  `say_queue_count=0` e `ESP_OK`. Em turno real via `/debug/transcript`, o
  downlink adicionou 283 chunks SAY recebidos e 283 tocados, com fila final
  zero, `say_chunks_dropped=0`, `say_chunks_dropped_listening=0`,
  `say_cancel_count=0`, `tts_completed=true` e `tts_say_end_sent=true`.
  `codec-v2 health` voltou a `healthy=true/status=ok` apos reativar o
  transporte Opus v2 pos-flash; `capture-v2 status` permaneceu desligado em
  `IDLE_SESSION`.
- Regressao assistida de barge-in pos-handoff: `/ai/metrics` registrou
  `turn_id=11`, `outcome=interrupted`, `discard_reason=barge_in` e
  `interruption_cancel=3.5 ms`. O status seguinte de Playback v2 ficou com
  `bridge_say_queue_owner=true`, `say_queue_count=0`, `last_error=ESP_OK` e
  drops de SAY durante listening contabilizados como descarte esperado de audio
  antigo. Observacao: o CLI `barge-live` saiu por timeout porque o ultimo turno
  ja tinha avancado quando ele consultou `/ai/metrics`, mas a interrupcao real
  foi registrada.
- Validacao controlada pos-restart em hardware: o server foi reiniciado limpo,
  reconectou ao firmware, o transporte Opus v2 foi reativado e `/ai/status`
  confirmou `audio.format=opus`, `codecs.opus=true`, modelo `qwen3.5:9b` e
  STT/LLM/TTS `ok`. O teste manual `ww -> que horas sao? -> ww -> me conte
  uma historia longa -> ww -> pare` passou de ponta a ponta: `turn_id=4`
  reconheceu `Que horas são?` como `local_time`; `turn_id=5` registrou
  `outcome=interrupted` e `discard_reason=barge_in`; `turn_id=6` reconheceu
  `Pare.` e respondeu `Opa, parei! O que houve?`. A latencia de cancelamento
  ficou em `interruption_cancel` p50 2,6 ms / p95 3,2 ms, Playback v2 terminou
  com `bridge_say_queue_owner=true`, `say_queue_count=0`,
  `say_cancel_count=2`, `say_chunks_cancelled=28` e `last_error=ESP_OK`.
  O usuario confirmou audivelmente que funcionou perfeitamente.

Aceite:

- TTS normal fala ate `SAY_END` e volta a `IDLE`.
- Barge-in por wake cancela playback e nao toca audio antigo.
- `no-echo-live` segue sem turno fantasma.
- `TEXT_SCROLL` continua sincronizado ao audio o suficiente para nao vender
  texto que o robo nao falou.
- PCM16 e Opus upstream continuam iguais; esta fase nao mexe em codec.

Rollback:

- Desligar a rota de playback v2 e manter SAY pelo caminho atual do
  `audio_service`.

## Fase J - Voice Activity v2 / AFE-VAD-NS Opt-in

Objetivo: criar o processor plugavel do NoiseBot, usando AFE/VAD/NS
como diagnostico ou opt-in, sem prometer AEC.

Entregas:

- Primeiro incremento local implementado: `voice_activity_service_v2` agora
  possui shadow probe passivo alimentado por copia do PCM condicionado do
  `audio_service`, com `GET /api/audio/activity-v2`,
  `POST /api/audio/activity-v2/shadow` e
  `POST /api/audio/activity-v2/shadow/stop`. Ele mede RMS/peak, fala/silencio
  por limiares conservadores, frames mutados por playback e sessao ativa,
  mas nao abre sessao, nao chama bridge, nao chama wake e nao acessa HAL.
- Validacao em hardware apos flash: status inicial de `/api/audio/activity-v2`
  retornou `initialized=true`, `shadow_running=false` e `ESP_OK`. Um shadow de
  1000 ms observou 63 frames, encerrou sozinho em 1008 ms, classificou
  silencio (`speech_frames=0`, `silence_frames=63`), registrou `rms_max=584`,
  `peak_max=1120`, `muted_frames=0` e manteve `session_active=false`. Playback
  v2 seguiu com fila SAY zero e capture-v2 desligado. Apos reativar Opus v2
  pos-flash, `codec-v2 health` voltou `healthy=true/status=ok`, worker
  `running`, zero drops e `opus_codec_error=0`.
- `voice_activity_service_v2` com status, probes e comparacao entre VAD atual,
  AFE/VAD e metricas RMS/ZCR/espectral.
- Modo shadow que observa a sessao real sem decidir wake nem fim de fala.
- Eventos internos de `speech_start`, `speech_end`, `silence`, `discard_reason`
  apenas quando existe sessao aberta.
- Gate explicito: AEC device-side bloqueado se `input_reference=false`.

Aceite:

- VAD v2 nao abre sessao sozinho em `IDLE`.
- Wake word atual nao muda.
- Barge-in por wake continua ok.
- No-echo continua ok.
- Qualquer ganho em falso positivo/falso negativo precisa aparecer em replay ou
  harness, nao em percepcao solta.

Rollback:

- Voltar ao VAD/heuristica atual e manter AFE apenas como probe.
- Para este primeiro incremento, rollback e remover/desabilitar a chamada
  `voice_activity_service_v2_feed_frame()` no `audio_service`; como o probe e
  passivo, wake/captura/playback/codec continuam no caminho ja validado.

## Fase K - Capture Session v2 Como Dono Gradual do Upstream

Objetivo: mover pre-roll, timeouts, razao de descarte e envio de
`VOICE_START/AUDIO_CHUNK/VOICE_END` para `voice_capture_session_v2`, sem mudar
quem pode abrir uma conversa.

Entregas:

- Handoff opt-in do `audio_service` para `voice_capture_session_v2`.
- Pre-roll v2 real com supressao correta em barge-in.
- Timeouts e `end_reason` padronizados.
- Regras preservadas: wake vazio nao envia STT; `VOICE_END` so sai se houve
  `VOICE_START` e audio.
- Bridge TX passando por contrato v2 observavel.

Aceite:

- Turno curto, turno longo, silencio apos wake, barge-in e no-echo passam em
  PCM16 e Opus.
- `capture-v2 status` explica sessao ativa, estado, source, samples, drops e
  ultimo erro.
- O caminho antigo continua disponivel por flag.

Rollback:

- Desligar `voice_audio_v2_capture_enabled` e manter captura real no
  `audio_service`.

## Fase L - Policy Conversacional e Turn-taking Avancado

Objetivo: melhorar naturalidade sem reabrir o risco de responder conversa
ambiente.

Entregas:

- Contrato explicito entre `voice_controller`, state machine e sessao v2.
- Follow-up automatico apenas como opt-in com janela curta, telemetria e
  abort/cancel claros.
- Barge-in sem wake permanece bloqueado ate AFE/AEC/no-echo terem criterio
  proprio.
- Mapeamento consistente entre `IDLE`, `ATTENTIVE`, `RESPONDING`,
  `LISTENING/CAPTURING` e eventos de audio.

Aceite:

- O robo nao responde conversa da casa sem wake ou modo explicitamente armado.
- Wake durante fala aborta a fala velha e abre captura limpa.
- Sem loops de follow-up.
- Baseline `IDLE` continua mandatorio ao fim do turno.

Rollback:

- Desativar follow-up/turn-taking avancado e manter wake manual + barge-in por
  wake.

## Fase M - Release Checklist e Observabilidade Continua

Objetivo: transformar o que aprendemos em barreira de regressao antes de
qualquer mudanca grande.

Status parcial em 2026-06-01: checklist/health de release documentado em
`docs/VOICE_AUDIO_V2_RELEASE_CHECKLIST.md`, sem alterar firmware C. O escopo
protege Opus v2, Playback v2 como dono da fila SAY, Capture v2 desligado,
barge-live/no-echo-live e completude TTS/texto. Wake, VAD, AEC, follow-up,
`audio_service.c` e HAL permanecem fora desta fase parcial.

Entregas:

- Checklist unico de voz para release local.
- Health gates para `codec-v2`, captura, playback, barge/no-echo e completude
  TTS/texto.
- Replays com amostras reais boas e ruins.
- Testes de reconexao/cancelamento explicito no bridge.
- Registro claro de codec ativo, drops, filas, STT, TTS, SAY e estado final.

Comandos base:

```powershell
noisebot_server debug codec-v2 health --json
noisebot_server debug codec-ab --repeat 3 "me diga uma curiosidade" --json
noisebot_server debug barge-live "me conte uma historia longa" --codec opus-v2 --json
noisebot_server debug no-echo-live "me conte uma historia longa" --codec opus-v2 --json
noisebot_server debug capture-v2 status --json
```

Gates obrigatorios da Fase M parcial:

- Escopo limpo: nenhum diff em firmware C, `audio_service.c`, HAL, wake, VAD,
  AEC ou follow-up.
- Codec v2: `codec-v2 health` com `healthy=true`, `status=ok`, zero drops,
  fila egress zero, `opus_codec_error=0` e rollback PCM16 documentado.
- Playback v2 fila SAY: `/api/audio/playback-v2` com
  `bridge_say_observer=true`, `bridge_say_queue_owner=true`, fila final zero,
  chunks recebidos/tocados coerentes e sem drops novos no caminho do
  orquestrador.
- Capture v2 desligado: `real_capture_enabled=false`, `session_active=false`,
  `state=IDLE_SESSION`, `ESP_OK`.
- Turn-taking: `barge-live --codec opus-v2` interrompe por wake com
  `discard_reason=barge_in`; `no-echo-live --codec opus-v2` nao abre turno
  fantasma.
- TTS/texto: `/ai/metrics` separa `tts_completed=false`, falha de `SAY_END`,
  truncamento visual e paginacao `TEXT_SCROLL`; `text_scroll_pages_sent` deve
  cobrir `text_scroll_pages` quando houver paginacao.

Aceite:

- Nenhuma fila/drops/erro de codec fica ambigua.
- Corte visual, corte de audio e falha de TTS ficam separados em metricas.
- Toda regressao real vira caso de teste antes de novo ajuste.
- PCM16 permanece rollback operacional por env/restart ou
  `codec-v2 transport-disable`.

## Ordem Recomendada

1. Fase I: playback v2 como dono gradual do downlink.
2. Fase M parcial: checklist/health de release para proteger o que ja ficou
   bom.
3. Fase J: voice activity v2 em shadow/opt-in, sem AEC.
4. Fase K: capture session v2 assume upstream por flag.
5. Fase L: policy conversacional avancada, so depois de no-echo e captura
   estarem estaveis.

Essa ordem segue a regra central da arquitetura v2: separar I/O, playback,
processor, codec e policy. No NoiseBot, a prioridade imediata e diminuir o
acoplamento do `audio_service.c` sem trocar wake, VAD, AEC e follow-up no mesmo
movimento.
