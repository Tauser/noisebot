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
- Observacao pos-handoff: uma resposta curta com 395 chunks TTS gerou engasgos
  audiveis. `/ai/metrics` mostrou TTS completo (`tts_completed=true`,
  `tts_say_end_sent=true`), enquanto `/api/audio/playback-v2` mostrou 252
  chunks aceitos/tocados e 143 drops novos. A causa ficou no pacing do server:
  o `OutputScheduler` fazia catch-up em rajada depois de pausas entre sentencas
  do TTS. Correcao server-only: manter prebuffer curto e enviar chunks extras
  em cadencia de 16 ms, sem rajada. Validacao local: `server/tests` 155 verdes.
- Validacao real apos restart do server confirmou a correcao: baseline
  `/api/audio/playback-v2` em `received=494`, `played=494`, `dropped=154`;
  depois de `ww -> me conte uma historia curta`, o endpoint ficou em
  `received=892`, `played=892`, `dropped=154`. O turno teve 398 chunks TTS,
  `tts_completed=true`, `tts_say_end_sent=true`, `voice_alert=null`, Capture v2
  desligado e Codec v2 sem drops apos `egress-drain`.

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
- Incremento local seguinte: Activity v2 passou a expor `zcr_last_permille` e
  `zcr_max_permille` no status do shadow. O calculo usa cruzamentos de zero
  por mil amostras, sem float e sem alocacao, apenas como telemetria para
  futura comparacao de fala/ruido; nao entra em politica de wake/fim de fala.
- Validacao em hardware apos flash do ZCR: shadow de 1000 ms observou 63 frames,
  encerrou sozinho em 1008 ms, manteve `session_active=false`, classificou
  silencio, registrou `zcr_last_permille=98` e `zcr_max_permille=141`; `codec-v2
  health` voltou `healthy=true/status=ok` apos reativar Opus v2.
- Incremento local seguinte: Activity v2 passou a separar a telemetria do
  shadow por contexto, expondo `session_frames`, `idle_frames`,
  `muted_frames`, `unmuted_frames` e maximos RMS/peak/ZCR separados para frames
  mutados por playback e nao mutados. Isso prepara a validacao comparativa
  durante `ww -> resposta -> ww`, sem transformar o shadow em decisor de
  wake, fim de fala, captura ou cancelamento.
- Incremento local seguinte: a validacao manual em turno real mostrou que a
  janela de 10 s e o mute baseado apenas no chunk escrito no speaker podiam
  deixar o shadow classificar tudo como `idle/unmuted`, mesmo com resposta
  falada. O limite do shadow subiu para 30 s e o `audio_service` agora passa ao
  Activity v2 um contexto explicito de playback (`wrote_audio`, estado
  `PLAY_ACTIVE/PLAY_BRIDGE_SAY`, `bridge_say_playing` e fila SAY v2 tocando).
  Essa informacao alimenta apenas os buckets de telemetria; VAD, wake,
  captura, codec, Playback v2 e HAL nao mudam.
- Validacao em hardware apos flash do shadow 30 s: com Opus v2 reativado,
  `ww -> me conte uma historia curta` gerou transcript `Me conte uma história
  curta.`, `tts_completed=true`, `tts_say_end_sent=true`, `voice_alert=null` e
  `codec-v2 health` ok. O Activity v2 encerrou o shadow com 1875 frames em
  30000 ms, `session_frames=268`, `idle_frames=1607`, `muted_frames=478` e
  `unmuted_frames=1397`, confirmando que a telemetria agora enxerga sessao e
  playback. Ponto amarelo: Playback v2 registrou 384 chunks SAY
  recebidos/tocados e 14 drops no turno; repetir antes de qualquer novo handoff
  de playback/captura.
- Repeticao controlada via orquestrador apos restart correto do server com
  `NOISEBOT_HOST=192.168.1.30` e Opus v2 ativo: `/debug/transcript` com
  `me conte uma historia curta` enviou 292 chunks TTS, `SAY_END=true`,
  `voice_alert=null`, `codec-v2 health` ok e `capture-v2` desligado. Playback
  v2 saiu de `received=1139/played=1138/dropped=38` para
  `received=1431/played=1430/dropped=38`, ou seja, +292 recebidos/tocados e
  zero drops novos.
- Repeticoes fisicas por wake: uma repeticao curta gerou +222 chunks SAY e
  zero drops novos, mas transcreveu `E ai?` em vez do comando pretendido. A
  repeticao seguinte transcreveu `Me fala em historia curta.`, respondeu com
  292 chunks TTS completos e `SAY_END`, mas Playback v2 saiu de
  `received=2295/played=2292/dropped=38` para
  `received=2569/played=2566/dropped=56`, ou seja, +274 chunks tocados e
  +18 drops. Conclusao: o caminho controlado esta limpo, mas o caminho fisico
  ainda pode encher a fila SAY na transicao wake -> resposta. Ajuste
  server-only atual: reduzir o prebuffer padrao `NOISEBOT_TTS_QUEUE_TARGET`
  de 12 para 6 chunks, deixando mais headroom na fila de 16 chunks do
  firmware antes de liberar novo handoff.
- Validacao fisica apos reduzir o prebuffer para 6 chunks: `ww -> me diga uma
  fala com historia curta` transcreveu `Me diga uma fala com história curta.`,
  `transcript_quality=good`, `tts_chunks_sent=326`, `tts_completed=true`,
  `tts_say_end_sent=true`, `voice_alert=null` e texto visual completo em 3
  paginas. Playback v2 saiu de `received=2569/played=2566/dropped=56` para
  `received=2895/played=2892/dropped=56`, ou seja, +326 chunks recebidos e
  tocados com zero drops novos. `codec-v2 health` ficou `healthy=true/status=ok`
  e `capture-v2` permaneceu desligado em `IDLE_SESSION`.
- Refino atual de headroom: fila SAY estatica do Playback v2 ampliada para 32
  chunks, server sem prebuffer inicial por default
  (`NOISEBOT_TTS_QUEUE_TARGET=0`) e pacing conservador de 18 ms por chunk
  (`NOISEBOT_TTS_SEND_INTERVAL_MS=18`). Objetivo: reduzir risco de encher a
  fila SAY na transicao wake -> resposta sem tocar wake, captura, codec ou HAL.
- Validacao fisica de resposta mais longa: `ww -> me conte uma história um
  pouco mais longa` gerou `tts_chunks_sent=825`, `tts_completed=true`,
  `tts_say_end_sent=true`, `voice_alert=null` e 7 paginas visuais. Playback v2
  saiu de `received=2895/played=2892/dropped=56` para
  `received=3720/played=3717/dropped=56`, ou seja, +825 chunks recebidos e
  tocados com zero drops novos. O health do Codec v2 apontou 1 pacote egress
  pendente, drenado por `codec-v2 egress-drain`, e voltou `status=ok`.
- Validacao fisica de barge-in: `ww -> me conte uma história longa -> ww ->
  pare` registrou `outcome=interrupted`, `discard_reason=barge_in` e
  `interruption_cancel=3.3 ms`. Playback v2 terminou com fila zero,
  `say_cancel_count` +1 e `say_chunks_cancelled` +5; os +6 drops ocorreram em
  `say_chunks_dropped_listening`, ou seja, descarte esperado de audio antigo
  durante a nova escuta. Observacao visual descoberta: apos o cancelamento, o
  display ainda podia manter texto da resposta anterior. Correcao firmware:
  `SPEECH_CANCEL` e `LISTEN_START` limpam `ui_overlay_clear_text()` antes de
  mostrar `Ouvindo...`.
- Validacao fisica pos-correcao do stop curto: ao repetir
  `ww -> historia longa -> ww -> pare`, o STT ainda confundiu o comando final
  com `Vale.`, mas o server tratou corretamente como `local_stop` por estar
  dentro da janela de barge-in recente. `/ai/status` retornou
  `last_outcome=local_intent`, `last_reply="Pronto, parei."`; `/ai/metrics`
  confirmou `intent_name=local_stop`, `tts_completed=true`,
  `tts_say_end_sent=true`, `discard_reason=null` no turno de stop e o turno
  anterior como `outcome=interrupted` / `discard_reason=barge_in`. Playback v2
  ficou com `say_queue_count=0`, `last_error=ESP_OK`; `capture-v2` seguiu
  desligado e `codec-v2 health` voltou `status=ok` apos drenar 1 pacote egress
  pendente.
- Observacao operacional: a queda percebida do server nesta rodada nao apontou
  para crash do `OutputScheduler`. O log mostrou um start sem
  `NOISEBOT_HOST`, que deixa `/ai/status` em `connected=false` e o server sem
  transporte TCP; o `.env` local ignorado pelo git foi corrigido com
  `NOISEBOT_HOST=192.168.1.30`. Ao iniciar por comando manual, usar tambem
  `--host 192.168.1.30` ou exportar a variavel.
- Modo shadow que observa a sessao real sem decidir wake nem fim de fala.
- Eventos internos de `speech_start`, `speech_end`, `silence`, `discard_reason`
  apenas quando existe sessao aberta.
- Incremento local atual: Activity v2 agora expõe sequencias consecutivas
  passivas de fala/silencio no shadow (`speech_run_frames`,
  `silence_run_frames`, `speech_run_max_frames`,
  `silence_run_max_frames`). Esses campos servem para comparar estabilidade de
  VAD/end-of-speech em testes futuros; nao abrem sessao, nao alteram wake, nao
  enviam bridge e nao chamam HAL. Validacao local: contrato focado
  `test_voice_activity_v2_shadow_is_explicit_and_passive` passou e
  `idf.py build` ficou limpo, com 34% livre na menor particao.
  Validacao em hardware apos flash: status inicial mostrou os novos campos
  zerados; shadow padrao de 1000 ms encerrou em 1008 ms com 63 frames,
  `state=SILENCE`, `speech_frames=0`, `silence_frames=63`,
  `speech_run_max_frames=0`, `silence_run_frames=63`,
  `silence_run_max_frames=63`, `idle_frames=63`, `unmuted_frames=63`,
  `last_error=ESP_OK`. Playback v2 ficou com fila zero, Capture v2 desligado e
  `codec-v2 health` `status=ok` apos reativar Opus v2.
- Validacao real de 30 s durante turno por wake: shadow rodou durante
  `ww -> me conte uma historia curta`, e `/ai/metrics` confirmou transcript
  `Me conte uma história curta.`, `outcome=llm`, `tts_completed=true`,
  `tts_say_end_sent=true`, `tts_chunks_sent=253` e `voice_alert=null`.
  Activity v2 encerrou com `observed_frames=1875`, `session_frames=384`,
  `idle_frames=1491`, `muted_frames=334`, `unmuted_frames=1541`,
  `speech_frames=45`, `silence_frames=1830`, `speech_run_max_frames=7` e
  `silence_run_max_frames=521`, provando que os runs enxergam fala/silencio,
  sessao e playback mutado em turno real. Playback v2 ficou com fila zero e
  zero drops novos (`received=1030`, `played=1030`, `dropped=0`), Capture v2
  permaneceu desligado e `codec-v2 health` ficou `status=ok`.
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
- Primeiro incremento local: `capture-v2 status` ganhou `end_reason`,
  `bridge_tx_owner` e `legacy_audio_service_tx_owner`, deixando explicito que
  o Capture v2 ainda observa/acompanha a sessao enquanto o envio real ao bridge
  permanece no `audio_service`. Isso nao altera wake, VAD, codec, playback,
  bridge TX real nem HAL; prepara a troca de ownership com rollback claro.
- Validacao em hardware apos flash: `/api/audio/capture-v2` expos os novos
  campos; replay diagnostico `speech_ms=640`, `silence_ms=900` encerrou em
  `DONE`, `end_reason=SPEECH_COMPLETE`, `voice_start_sent=true`,
  `voice_audio_sent=true`, `voice_end_sent=true`, `captured_samples=10240`,
  `dropped_frames=0`, `bridge_tx_owner=false` e
  `legacy_audio_service_tx_owner=true`. Apos reativar Opus v2, `codec-v2
  health` voltou `status=ok`, worker `running`, zero drops e fila egress zero;
  Playback v2 permaneceu com fila SAY zero.
- Incremento local seguinte: `capture-v2 status` passou a expor o espelho
  shadow do TX legado (`shadow_voice_start_sent`, `shadow_voice_end_sent`,
  `shadow_audio_chunks`, `shadow_audio_samples` e
  `shadow_audio_dropped_chunks`). Esses campos contam o ponto em que o Capture
  v2 emitiria `VOICE_START/AUDIO_CHUNK/VOICE_END`, mas o envio real continua
  100% no `audio_service`; `bridge_tx_owner` permanece `false`.
- Validacao em hardware apos flash do shadow TX: replay diagnostico
  `speech_ms=640`, `silence_ms=900` retornou `state=DONE`,
  `end_reason=SPEECH_COMPLETE`, `shadow_voice_start_sent=true`,
  `shadow_voice_end_sent=true`, `shadow_audio_chunks=40`,
  `shadow_audio_samples=10240`, `shadow_audio_dropped_chunks=0`,
  `captured_samples=10240`, `dropped_frames=0`, `bridge_tx_owner=false` e
  `legacy_audio_service_tx_owner=true`. Opus v2 foi reativado em seguida e
  `codec-v2 health` voltou `status=ok`, worker `running`, zero drops e fila
  egress zero.
- Validacao real inicial com `voice_audio_v2_capture_enabled=true` confirmou
  que o caminho legado continuou dono do TX (`bridge_tx_owner=false`) e o
  turno concluiu com `voice_alert=null`, Playback v2 sem drops e Codec v2 ok.
  A mesma rodada revelou um desvio de unidade no shadow Opus: os 158 eventos
  batiam com `chunk_count=158`, mas `shadow_audio_samples` somava 158 * 256 em
  vez dos 158 pacotes Opus * 960 samples recebidos pelo server. Correcao local:
  `bridge_drain_opus_packets_if_enabled()` agora retorna quantidade de pacotes
  drenados e o shadow soma `sent_packets * NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES`.
- Revalidacao apos flash da correcao de samples: turno real
  `ww -> me diga uma curiosidade curta` retornou `transcript_quality=good`,
  `voice_alert=null`, Playback v2 sem drops, Codec v2 ok,
  `shadow_audio_samples=52800` contra `total_samples=52784` no server
  (diferenca de 16 samples por alinhamento). A telemetria mostrou que
  `shadow_audio_chunks` e `speech_elapsed_ms` ainda refletiam chamadas do loop
  de 16 ms; correcao local seguinte passou a contabilizar unidades de frame
  Opus e elapsed por `sample_count`.
- Revalidacao final apos flash do refinamento de unidades: turno real
  `ww -> me diga uma curiosidade curta` retornou `transcript_quality=good`,
  `outcome=local_intent`, `voice_alert=null`, Playback v2 recebeu/tocou 205
  chunks SAY com zero drops, Codec v2 `status=ok`, e Capture v2 ficou
  `DONE` com `bridge_tx_owner=false`, `legacy_audio_service_tx_owner=true`,
  `shadow_audio_chunks=58` contra `chunk_count=58`,
  `shadow_audio_samples=55680` contra `total_samples=55664`,
  `speech_elapsed_ms=3480` e `shadow_audio_dropped_chunks=0`. A diferenca de
  16 samples permanece apenas alinhamento de frame; o shadow TX Opus esta
  coerente.
- Incremento local seguinte: `/api/audio/capture-v2` passa a expor o gate de
  handoff `bridge_tx_candidate`, `bridge_tx_handoff_ready` e
  `handoff_block_reason`. Esses campos sao apenas status: indicam quando uma
  sessao real observada pela Capture v2 estaria pronta para virar candidata ao
  ownership de `VOICE_START/AUDIO_CHUNK/VOICE_END`, ou por que ainda nao esta
  (`NOT_REAL_CAPTURE`, `SESSION_ACTIVE`, `NO_AUDIO`, `DROPPED_AUDIO`,
  `END_REASON`, `ALREADY_OWNER`). O bridge TX real continua no
  `audio_service`.
- Validacao em hardware apos flash do gate: replay diagnostico ficou bloqueado
  corretamente por `handoff_block_reason=NOT_REAL_CAPTURE`; em seguida, turno
  real `ww -> me diga uma curiosidade curta` ficou `DONE` com
  `real_capture=true`, `bridge_tx_candidate=true`,
  `bridge_tx_handoff_ready=true`, `handoff_block_reason=NONE`,
  `shadow_audio_chunks=158`, `shadow_audio_samples=151680`,
  `shadow_audio_dropped_chunks=0`, Playback v2 com 254 chunks
  recebidos/tocados e zero drops, Codec v2 `status=ok` e
  `opus_codec_error=0`. Ponto amarelo: o server encerrou o turno por timeout
  (`duration_ms=9479`, `voice_end_reason=timeout`), nao por silencio; antes do
  handoff real, repetir pelo menos um turno curto que finalize por silencio.
- Repeticao curta por silencio passou: `ww -> que horas sao` gerou transcript
  `Que horas são?`, `voice_end_reason=silence`, `duration_ms=3719`,
  `chunk_count=62`, `total_samples=59504`, `tts_completed=true`,
  `tts_say_end_sent=true` e `voice_alert=null`. Capture v2 ficou
  `bridge_tx_candidate=true`, `bridge_tx_handoff_ready=true`,
  `handoff_block_reason=NONE`, `shadow_audio_chunks=62`,
  `shadow_audio_samples=59520`, zero drops; Playback v2 somou 404 chunks
  recebidos/tocados sem drops novos. Codec v2 apontou 1 pacote egress pendente
  sem erro/drop, `egress-drain` drenou esse pacote e `codec-v2 health` voltou
  `status=ok`.
- Incremento local de preparacao do handoff real: nova flag NVS
  `voice_audio_v2_capture_tx_enabled`, default `false`, exposta em
  `/api/config`, `/api/config/all`, `/api/audio/capture-v2` como
  `bridge_tx_handoff_enabled` e no CLI
  `noisebot_server debug capture-v2 tx-enable|tx-disable`. Este passo ainda
  nao altera o envio real de `VOICE_START/AUDIO_CHUNK/VOICE_END`; ele apenas
  cria o arm/disarm operacional separado da flag de observacao
  `voice_audio_v2_capture_enabled`.
- Validacao pos-flash da flag de handoff: default novo apareceu desligado
  (`bridge_tx_handoff_enabled=false`), `tx-enable` e `tx-disable` alternaram a
  NVS corretamente, e um turno real curto com a flag novamente desligada manteve
  `bridge_tx_owner=false`, `legacy_audio_service_tx_owner=true`,
  `bridge_tx_candidate=true`, `bridge_tx_handoff_ready=true`,
  `handoff_block_reason=NONE`, zero drops em Capture/Playback e transcript
  `Que horas sao?` com `voice_end_reason=silence`. Um pacote egress Opus
  pendente foi drenado e `codec-v2 health` voltou `status=ok`.
- Correcao local de infraestrutura HTTP: o firmware tinha 98 rotas mas
  `max_uri_handlers=64`, fazendo APIs tardias como `/api/config/all` poderem
  responder 404 apesar de existirem na tabela. O limite agora deriva de
  `k_uris` com margem e loga falha de registro por rota. Pos-flash,
  `/api/config/all` voltou a responder JSON e confirmou
  `voice_audio_v2_capture_enabled=true` com
  `voice_audio_v2_capture_tx_enabled=false`.
- Turno curto real apos esse reflash tambem passou: `ww -> que horas sao`
  gerou transcript `Que horas são?`, `voice_end_reason=silence`,
  `duration_ms=2159`, `chunk_count=36`, `total_samples=34544`,
  `tts_completed=true`, `tts_say_end_sent=true` e `voice_alert=null`.
  Capture v2 ficou com `bridge_tx_owner=false`,
  `legacy_audio_service_tx_owner=true`, `bridge_tx_handoff_ready=true`,
  `handoff_block_reason=NONE`, `shadow_audio_chunks=36`,
  `shadow_audio_samples=34560` e zero drops; Codec v2 voltou `status=ok`.
  Playback v2 esta operacional, mas seus drops sao cumulativos de interacoes
  anteriores e devem ser medidos por delta antes do proximo handoff real.
- Para essa medicao, o server agora expoe o harness
  `noisebot_server --host 192.168.1.30 debug playback-v2 status|delta --json`.
  O modo `delta` captura snapshots antes/depois de um turno real e calcula
  deltas de recebidos, tocados, drops normais, drops durante listening,
  cancelados e cancels. Sanity check local em hardware, sem turno entre
  snapshots, retornou `queue_empty=true`, `normal_path_clean=true` e todos os
  deltas zero.
- Incremento local de handoff real opt-in: quando
  `voice_audio_v2_capture_enabled=true` e
  `voice_audio_v2_capture_tx_enabled=true`, o `audio_service` continua dono do
  HAL/mic e do condicionamento de audio, mas roteia `VOICE_START`,
  `AUDIO_CHUNK` PCM16/Opus e `VOICE_END` por funcoes explicitas de
  `voice_capture_session_v2`. Com a flag de TX desligada, o caminho legado
  permanece exatamente como antes e o Capture v2 segue shadow. Validacao local:
  contrato focado Voice Audio v2 e build ESP-IDF limpos. Falta flash e teste
  fisico antes de considerar o handoff validado.
- Validacao fisica apos flash do handoff real opt-in passou em turno curto:
  apos reativar Opus e ligar `capture-v2 tx-enable`, `ww -> que horas sao`
  gerou `turn_id=47`, transcript `Que horas são?`, `voice_end_reason=silence`,
  `chunk_count=104`, `total_samples=99824`, `tts_completed=true`,
  `tts_say_end_sent=true` e `voice_alert=null`. O status do Capture v2 ficou
  `bridge_tx_owner=true`, `legacy_audio_service_tx_owner=false`,
  `end_reason=SPEECH_COMPLETE`, `shadow_audio_chunks=104`,
  `shadow_audio_samples=99840` e zero drops. Playback v2 ficou com fila zero e
  zero drops; Codec v2 teve 1 pacote egress pendente sem erro/drop, foi drenado
  e voltou `status=ok`. A flag experimental de TX foi desligada em seguida;
  `/api/config/all` confirmou `voice_audio_v2_capture_tx_enabled=false`.
- Validacao fisica de barge-in com handoff real opt-in tambem passou: com
  `capture-v2 tx-enable`, o roteiro
  `ww -> me conte uma historia longa -> ww -> pare` gerou o turno 49 como
  `outcome=interrupted`, `discard_reason=barge_in` e transcript
  `Me conte uma história longa.`; em seguida o turno 50 reconheceu `Pare.`
  como `intent_name=local_stop`, respondeu `Pronto, parei.`, completou TTS e
  enviou `SAY_END`. O status do Capture v2 durante o barge-in ficou
  `source=BARGE_IN`, `bridge_tx_owner=true`,
  `legacy_audio_service_tx_owner=false`, `end_reason=SPEECH_COMPLETE`,
  67 chunks / 64320 samples e zero drops. Playback v2 terminou com fila zero,
  1 cancelamento, 3 chunks cancelados e 7 drops classificados como
  `say_chunks_dropped_listening`, ou seja, descarte de audio velho durante a
  nova escuta. Codec v2 permaneceu `status=ok`. A flag experimental de TX foi
  desligada ao fim e `/api/config/all` confirmou
  `voice_audio_v2_capture_tx_enabled=false`.
- Endurecimento operacional local: desligar
  `voice_audio_v2_capture_tx_enabled` agora tambem libera o ownership interno
  do Capture v2 mesmo em idle, para que o rollback nao deixe o status parado em
  `bridge_tx_owner=true` da ultima sessao. Ligar ownership continua exigindo
  sessao real ativa. Validacao local: contrato focado Voice Audio v2 e build
  ESP-IDF limpos; validacao pos-flash confirmou rollback limpo com
  `bridge_tx_handoff_enabled=false`, `bridge_tx_owner=false` e
  `legacy_audio_service_tx_owner=true`.
- Aceite final do handoff opt-in da Fase K: apos o flash do endurecimento,
  `capture-v2 tx-enable` foi rearmado com Opus ativo. Um turno curto
  `ww -> que horas sao` gerou `turn_id=54`, `voice_end_reason=silence`,
  `tts_completed=true`, `tts_say_end_sent=true`, Capture v2 como dono real do
  TX (`bridge_tx_owner=true`, `legacy_audio_service_tx_owner=false`), 78 chunks
  / 74880 samples e zero drops; Playback v2 recebeu/tocou 401 chunks SAY com
  fila final zero e zero drops; `codec-v2 health` ficou `healthy=true/status=ok`.
- Revalidacao final de barge-in com handoff real: o turno da historia
  (`turn_id=57`) ficou `outcome=interrupted` com `discard_reason=barge_in`;
  o Capture v2 reportou `source=BARGE_IN`, `bridge_tx_owner=true`,
  `legacy_audio_service_tx_owner=false`, `end_reason=SPEECH_COMPLETE`, 113
  chunks / 108480 samples e zero drops. Playback v2 terminou com fila zero,
  2 cancelamentos, 12 chunks cancelados e 6 drops apenas em
  `say_chunks_dropped_listening`, coerentes com descarte de audio antigo
  durante a nova escuta. O comando final desta repeticao foi reconhecido como
  `local_farewell` em vez de `local_stop`; isso fica registrado como detalhe de
  policy/STT, nao como bloqueio da arquitetura de captura. `codec-v2 health`
  permaneceu `healthy=true/status=ok`, e `capture-v2 tx-disable` confirmou
  rollback para `bridge_tx_owner=false`.
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

- Primeiro incremento server-only: o `LocalIntentProvider` ampliou a janela de
  controle apos barge-in recente. Variantes curtas de despedida/confirmacao
  dentro dessa janela, incluindo a transcricao real `Tchup! Bye!`, agora viram
  `local_stop` e respondem `Pronto, parei.` sem chamar a LLM. Fora do contexto
  de barge-in, `Tchau.`/`Bye.` continuam despedida normal. Isso nao altera
  wake, VAD, AEC, Capture v2, Playback v2, Codec v2, bridge ou firmware.
  Validacao local: `server/tests/test_server_facade.py -k local_intent` e
  `server/tests` completos passaram.
- Incremento server-only seguinte: o vocabulário direto de stop/cancelamento
  agora inclui `corta`, `corta isso`, `para de falar`, `chega disso`,
  `nao quero mais` e `encerra`, alem de `pare`/`cancela`. Essas frases viram
  `local_stop` mesmo fora de barge-in; despedidas ambiguas continuam
  contextuais. Validacao local: intents focados e `server/tests` completos.
- Incremento server-only de observabilidade: cada turno agora registra em
  `/ai/metrics.last_voice_session` os campos `recent_barge_in` e
  `turn_taking_policy` (`normal` ou `post_barge_in`). Em seguida, a telemetria
  ganhou `turn_taking_decision` (`direct_stop`, `post_barge_stop`,
  `local_intent` ou `llm`), deixando auditavel por que uma
  despedida/confirmacao curta foi tratada como controle de interrupcao. O
  `voice_diagnosis` tambem traduz `direct_stop` e `post_barge_stop` em
  diagnostico operacional, sem alterar wake, audio, codec, playback, captura ou
  firmware. Validacao local: testes focados de metricas/orquestrador/diagnostico
  e `server/tests` completos.
- Incremento server-only de painel: o dashboard operacional agora exibe
  `turn_taking_policy` e `turn_taking_decision` no diagnostico de voz, e o
  historico recente prioriza `turn_taking_decision` antes de descarte/intencao.
  Isso permite validar a Fase L sem abrir o JSON bruto de `/ai/metrics`.
- Incremento server-only de safety conversacional: follow-up automatico virou
  opt-in por `NOISEBOT_FOLLOWUP_ENABLED=false` por padrao, com janela limitada
  por `NOISEBOT_FOLLOWUP_WINDOW_MS` (1s-30s, default 8s quando ligado). Assim o
  robo nao rearma escuta por pergunta da propria resposta sem modo
  explicitamente habilitado. O dashboard operacional mostra o estado efetivo do
  follow-up e a janela configurada como leitura, sem habilitar o recurso por UI.
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

- Manter `NOISEBOT_FOLLOWUP_ENABLED=false` e usar wake manual + barge-in por
  wake. Para rollback de janela, remover `NOISEBOT_FOLLOWUP_WINDOW_MS` volta ao
  default de 8s quando o follow-up estiver ligado.

## Fase M - Release Checklist e Observabilidade Continua

Objetivo: transformar o que aprendemos em barreira de regressao antes de
qualquer mudanca grande.

Status fechado localmente em 2026-06-01: checklist/health de release
documentado em `docs/VOICE_AUDIO_V2_RELEASE_CHECKLIST.md`, sem alterar
firmware C. O escopo protege Opus v2, Playback v2 como dono da fila SAY,
Capture v2 desligado, barge-live/no-echo, rollback PCM16 e completude
TTS/texto. Wake, VAD, AEC, follow-up, `audio_service.c` e HAL permanecem fora
desta fase parcial.

Resultado final da rodada de hardware:

- `voice-release-check --json`: `ok=true`.
- Codec v2: `healthy=true`, `status=ok`, worker `running`, fila egress zero,
  drops zero e `opus_codec_error=0`.
- Capture v2: `real_capture_enabled=false`, `session_active=false`, ultima
  sessao retida em `DONE` apenas como diagnostico.
- Playback v2: fila SAY final zero, dono/observador ativo e `ESP_OK`.
- Turno curto, resposta longa, barge-in/pare e no-echo validados em hardware.
- No-echo foi aceito por `/ai/metrics` apos janela manual de 10 s: `turn_id=90`
  permaneceu como ultimo turno, sem wake vazio ou turno fantasma novo.
- Rollback PCM16 foi exercitado e Opus v2 religado com health final verde.
- Observacao: o helper interativo `no-echo-live --codec opus-v2` ficou preso no
  `input()` do TTY desta sessao; o comportamento de voz foi validado pelos
  endpoints usados pelo proprio helper.

Entregas:

- Checklist unico de voz para release local.
- Health gates para `codec-v2`, captura, playback, barge/no-echo e completude
  TTS/texto.
- Preflight agregado `voice-release-check` para consultar Codec v2, Capture v2,
  Playback v2 e `/ai/metrics` antes dos testes interativos. O mesmo agregado
  esta exposto em `GET /api/release/voice-check` no Ops HTTP e acionavel pelo
  botao `Release Check` do dashboard.
- Gate de cancelamento explicito coberto por testes automaticos: `SPEECH_CANCEL`
  remove fala pendente da fila TX, nao remove frames nao relacionados a fala, e
  o scheduler nao envia `SAY_END` artificial apos cancelamento.
- Replays com amostras reais boas e ruins: `docs/VOICE_REPLAY_BASELINE.json`
  referencia 2 comandos curtos aceitos e 2 amostras rejeitadas; o baseline e
  coberto por `bridge/tests/test_voice_check.py` e `bridge/tests/test_replay.py`.
- Testes de reconexao/cancelamento explicito no bridge: cancelamento explicito
  ja cobre `SPEECH_CANCEL`/turn id e reconexao TCP/UART cobre recriacao de
  transporte/adapter com backoff apos queda.
- Registro claro de codec ativo, drops, filas, STT, TTS, SAY e estado final:
  cada sessao gravada pelo server emite `VOICE_SESSION_FINAL` com JSON
  estruturado para auditoria, enquanto drops/filas seguem nos gates de Codec v2
  e Playback v2.
- Checklist manual de hardware para fechar release local: preflight, turno
  curto, resposta longa, barge-in/pare, no-echo e rollback PCM16 ficam
  documentados em `docs/VOICE_AUDIO_V2_RELEASE_CHECKLIST.md`.

Comandos base:

```powershell
noisebot_server debug codec-v2 health --json
noisebot_server debug voice-release-check --json
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
- Transporte: teste automatico de reconexao TCP/UART passa, cobrindo queda do
  adapter, desconexao do transporte antigo, recriacao do transporte/adapter e
  backoff entre tentativas.
- Replay offline: `bridge/tests/test_voice_check.py` e o baseline em
  `docs/VOICE_REPLAY_BASELINE.json` passam com fixtures reais boas/rejeitadas.
- Log final: cada sessao registrada pelo server emite `VOICE_SESSION_FINAL`
  com turno, rota, outcome, codec, STT/TTS, motivo final e estado.
- Hardware manual: rodada documentada cobre preflight, turno curto, resposta
  longa, barge-in/pare, no-echo e rollback PCM16 com resultados esperados e
  logs-chave.
- TTS/texto: `/ai/metrics` separa `tts_completed=false`, falha de `SAY_END`,
  truncamento visual e paginacao `TEXT_SCROLL`; `text_scroll_pages_sent` deve
  cobrir `text_scroll_pages` quando houver paginacao.

Aceite:

- Nenhuma fila/drops/erro de codec fica ambigua.
- Corte visual, corte de audio e falha de TTS ficam separados em metricas.
- Toda regressao real vira caso de teste antes de novo ajuste.
- PCM16 permanece rollback operacional por env/restart ou
  `codec-v2 transport-disable`.

## Fase N - Migracao Estrutural Do Voice v2 No Firmware

Objetivo: completar a transferencia gradual dos caminhos de audio em tempo real
para os servicos Voice Audio v2 do firmware, preservando rollback e mantendo
STT/LLM/TTS fora do ESP32-S3.

Estado de partida apos a Fase M:

- `audio_service.c` ainda e o dono real do loop I2S/HAL e continua drenando o
  playback para o speaker.
- `audio_playback_service_v2` ja e dono da fila SAY real, mas ainda nao escreve
  direto no HAL.
- `voice_capture_session_v2` ja tem TX real opt-in validado, mas ainda nao virou
  default permanente.
- `voice_activity_service_v2` ainda e shadow/telemetria, nao decisor principal
  de VAD/fim de fala.
- `audio_io_service_v2` ainda nao assumiu RX/TX real como servico principal.

### N0 - Baseline/Gates

Antes de qualquer mudanca estrutural:

- Rodar `voice-release-check --json`, `codec-v2 health --json`,
  `playback-v2 delta --json`, `capture-v2 status --json`,
  `barge-live --codec opus-v2` e validacao no-echo.
- Congelar snapshot saudavel com branch/hash, codec ativo, contadores SAY,
  estado Capture v2, `/ai/metrics.last_voice_session` e rollback PCM16.
- Aceite: Fase M continua verde sem novos warnings bloqueantes.

Status em 2026-06-01: N0 fechado em hardware no hash `0d97291`.

- `voice-release-check --json`: `ok=true`.
- Codec v2: `healthy=true`, `status=ok`, worker `running`, fila egress zero,
  drops zero e `opus_codec_error=0`.
- Playback v2 repouso: `playback-v2 delta --no-prompt --json` com deltas zero,
  `queue_empty=true` e `normal_path_clean=true`.
- Capture v2: `real_capture_enabled=false`, `session_active=false`,
  `bridge_tx_handoff_enabled=false`, `bridge_tx_owner=false`,
  `legacy_audio_service_tx_owner=true`; ultima sessao retida em `DONE` apenas
  como diagnostico.
- Barge-in: roteiro `ww -> me conte uma historia longa -> ww -> pare` validado
  por `/ai/metrics`; turno `94` ficou `outcome=interrupted` /
  `discard_reason=barge_in`, e turno `95` reconheceu `Pare.` como
  `local_stop`, com resposta `Pronto, parei.` e TTS completo.
- No-echo: turno `97` respondeu a `Me conte uma história longa.`, e apos
  janela manual de 10 s continuou como ultimo turno, sem wake vazio ou turno
  fantasma novo.
- Observacao operacional: os helpers interativos `barge-live` e `no-echo-live`
  ainda ficam presos no `input()` do TTY desta sessao; o aceite foi feito pelos
  endpoints usados pelos proprios helpers (`/ai/metrics`, Playback v2 e Codec
  v2).

### N1 - Capture v2 Default Controlado

Nao ligar direto como default permanente:

- Tornar `voice_capture_session_v2` default por config/flag persistente,
  reversivel e auditavel.
- Manter rollback claro para TX legado no `audio_service`.
- Aceite: turno curto, silencio pos-wake, barge-in/pare, no-echo, rollback
  PCM16 e Opus v2 health passam com Capture v2 como dono real do TX.

Inicio local em 2026-06-01:

- `voice_audio_v2_capture_tx_enabled=true` agora coabilita
  `voice_audio_v2_capture_enabled=true` no endpoint `/api/config`, evitando
  estado inconsistente em que o TX handoff fica armado sem uma sessao Capture v2
  real poder abrir.
- O rollback de TX continua em `capture-v2 tx-disable` /
  `voice_audio_v2_capture_tx_enabled=false`, que libera o ownership interno e
  devolve o TX logico ao caminho legado do `audio_service`; desligar tambem
  `voice_audio_v2_capture_enabled` preserva o rollback completo para shadow off.
- `voice-release-check` agora chama o gate de `Capture v2 controlado` e aceita
  dois estados saudaveis: baseline antigo desligado/inativo sem drops, ou N1
  com `real_capture_enabled=true`, `bridge_tx_handoff_enabled=true`, sessao
  inativa, erro `ESP_OK` e drops zero.

Status em 2026-06-02: N1 fechada em hardware no hash `9fa8b6b`.

- Pos-flash, `capture-v2 tx-enable` coabilitou corretamente
  `real_capture_enabled=true` e `bridge_tx_handoff_enabled=true`; Opus foi
  rearmado com `codec-v2 transport-enable`.
- Turno curto `ww -> que horas sao`: turno `102` fechou como `local_time`, TTS
  completo, `SAY_END`, texto `2/2`; Capture v2 foi dono real do TX
  (`bridge_tx_owner=true`, `legacy_audio_service_tx_owner=false`) com
  35 chunks / 33600 samples e zero drops. Apos drenar 1 pacote egress, Codec v2
  voltou `status=ok`.
- Barge-in/pare: a historia longa foi interrompida no turno `105`
  (`discard_reason=barge_in`) e o comando seguinte virou `local_stop` no turno
  `106`, com resposta `Pronto, parei.`. O STT transcreveu `Vale.`, mas a policy
  classificou corretamente. Capture v2 ficou `source=BARGE_IN`, dono real do TX,
  164 chunks / 157440 samples e zero drops; `voice-release-check` final ficou
  `ok=true` apos drenar 1 pacote egress Opus.
- No-echo: `ww -> diga oi` gerou turno `108`, resposta curta completa,
  TTS/SAY_END e texto `2/2`; apos a janela manual de silencio, o ultimo turno
  permaneceu `108`, sem turno fantasma. Capture v2 registrou 48 chunks /
  46080 samples e zero drops.
- Rollback PCM16: `codec-v2 transport-disable` retornou `opus_enabled=false`,
  health `status=ok`, worker parado, filas/drops zero e `voice-release-check`
  `ok=true`. Em seguida Opus foi reativado para manter o default local validado.
- Playback v2 ficou com fila zero; os warnings restantes sao contadores
  cumulativos de `say_chunks_dropped` e `say_chunks_dropped_listening` herdados
  dos cancelamentos/barge-in, sem bloquear gate.

### N2 - Activity v2 Como Decisor Dentro Da Sessao

Escopo restrito:

- `voice_activity_service_v2` so decide dentro de sessao ja aberta por wake ou
  barge-in.
- Nao abrir sessao em `IDLE`; wake/follow-up continuam donos da abertura.
- Comparar decisao v2 contra legado antes de promover, expondo divergencias em
  status/metrics.
- Aceite: fim de fala por Activity v2 bate o legado em turnos curtos/longos,
  nao cria turno fantasma e nao quebra barge-in.

Inicio local em 2026-06-02:

- `voice_activity_service_v2` ganhou um comparador de sessao real, iniciado pelo
  `audio_service` apenas quando uma sessao de escuta ja foi aberta por wake ou
  barge-in. Ele nao abre sessao em `IDLE`, nao chama bridge, nao chama HAL e nao
  decide o fim de fala ainda.
- O comparador observa os mesmos frames PCM16 condicionados ja enviados ao
  shadow, marca se a Activity v2 viu fala, quando ela teria encerrado por
  silencio (`activity_end_observed` / `activity_end_elapsed_ms`) e registra a
  decisao legada no encerramento (`legacy_end_observed`,
  `legacy_end_reason`, `legacy_end_elapsed_ms`).
- `/api/audio/activity-v2` agora expoe `session_compare_active`,
  `session_compare_id`, `session_compare_speech_seen`,
  `activity_end_observed`, `legacy_end_observed`, `decision_diverged` e tempos
  de decisao. O campo `decision_diverged` e apenas auditoria; o owner real do
  fim de fala continua sendo o VAD legado do `audio_service`.
- Validacao local: contrato focado Voice Audio v2 e build ESP-IDF limpos.
- Validacao fisica em 2026-06-02: apos calibrar o limiar do comparador de
  sessao para `RMS 200` / `peak 700`, o turno curto `ww -> que horas sao`
  passou com `speech_frames=15`, `session_compare_speech_seen=true`,
  `activity_end_observed=true`, legado por silencio e `decision_diverged=false`.
  O no-echo curto `ww -> diga oi` manteve o ultimo turno esperado, sem turno
  fantasma apos silencio, com `speech_frames=29` e `decision_diverged=false`.
  O barge/pare (`ww -> me conte uma historia` durante TTS `ww -> pare`) fechou
  com turno anterior `outcome=interrupted` / `discard_reason=barge_in`, comando
  seguinte `local_stop`, Capture v2 `source=BARGE_IN`, zero drops e Activity v2
  sem divergencia (`speech_frames=18`, `decision_diverged=false`). O pacote
  egress Opus residual foi drenado para fila zero.
- Status: N2 comparador passivo verde para turno curto, no-echo curto e
  barge/pare. Ainda nao promover Activity v2 a decisor real; o owner do fim de
  fala continua no VAD legado do `audio_service`.

### N3 - Audio IO v2 Assume RX/TX

Passo mais sensivel por tocar loop HAL/I2S:

- Primeiro RX mirror/owner com rollback.
- Depois TX/silencio controlado.
- Depois recovery/erro I2S com contadores claros.
- Aceite: zero regressao em wake, captura, playback, Opus/PCM16 e health de
  filas; qualquer I2S recovery fica observavel.

Status em 2026-06-02: N3 esta em andamento com RX distribuido e bridge TX
despachado pelo Audio IO v2, ainda sem handoff de HAL/TX.

- Incrementos locais fechados:
  - RX mirror/owner logico: `audio_io_service_v2` observa os frames reais de
    mic e expoe `rx_owner_*`, `rx_distributor_*` e comparacao de sessao.
  - Dispatcher RX generico: sound analysis, processor shadow, probe,
    session mirror, Activity v2, VAD legado e pre-roll passaram a consumir o
    frame via `audio_io_service_v2_rx_dispatch_frame`.
  - Bridge TX por dispatch: o envio real `VOICE_START/AUDIO_CHUNK/VOICE_END`
    continua sob `audio_service`/`voice_capture_session_v2`, mas o audio de mic
    que alimenta a bridge agora entra como consumidor do dispatcher RX, sem
    duplicar o bloco legado no loop principal.
- Validacao local: contrato focado Voice Audio v2 passou e `idf.py build`
  concluiu sem warnings.
- Validacao fisica apos flash do hash `9804179`: `/api/audio/io-v2` mostrou
  `rx_dispatch_last_consumers=8`; na sessao real, `session_rx_dispatch_calls=259`
  e `session_rx_dispatch_consumers=2072`, exatamente 8 consumidores por frame.
  Capture v2 ficou dono real do TX (`bridge_tx_owner=true`) com
  `voice_start/audio/end=true`, 69 chunks / 66240 samples e zero drops.
  Activity v2 comparou contra o legado com `decision_diverged=false`; Codec v2
  permaneceu `healthy=true`, worker `running`, drops zero; Playback v2 terminou
  com fila SAY zero. O comando final `Pare.` virou `local_stop` e respondeu
  `Pronto, parei.`.
- Ponto de atencao nao bloqueante: Playback v2 manteve contadores cumulativos
  de drops durante listening/cancelamento (`say_chunks_dropped_listening`), que
  sao coerentes com descarte de audio antigo em barge-in; devem ser avaliados
  por delta em novas rodadas.
- Incremento TX/silencio/recovery observado fechado no hash `8d22f38`:
  `audio_io_service_v2` passou a registrar `tx_owner_observed`,
  `tx_owner_frames`, `tx_owner_samples`, ultimo tamanho de TX, se o ultimo TX
  foi silencio e `tx_owner_last_result`; recuperacoes I2S agora incrementam
  `i2s_recoveries`. O `audio_service` continua sendo o unico dono do HAL e
  apenas anota o resultado de WAV, SAY, probe, synth e silencio no contrato v2.
- Validacao local do incremento TX: contrato focado Voice Audio v2 passou e
  `idf.py build` concluiu sem warnings.
- Validacao fisica apos flash do hash `8d22f38`: baseline pos-boot mostrou
  `tx_owner_observed=true`, `tx_owner_frames=3825`, `tx_frames=3825`,
  `tx_owner_last_result=ESP_OK`, `tx_owner_last_silence=true`,
  `i2s_recoveries=0` e `dropped_frames=0`. O teste real
  `ww -> que horas sao -> ww -> pare` manteve `rx_dispatch_last_consumers=8`,
  Capture v2 dono do TX real com zero drops, Playback v2 com fila final zero e
  zero drops, Activity v2 `decision_diverged=false`, e `Pare.` como
  `local_stop`. Depois de drenar 1 pacote egress residual, `codec-v2 health`
  voltou `status=ok`, sem warnings.
- Incremento de handoff de speaker em dry-run fechado nos hashes `c5da270` e
  `afd885c`: `/api/audio/io-v2` agora expoe `speaker_handoff_supported`,
  `speaker_handoff_dry_run_enabled`, `speaker_handoff_active`,
  `speaker_handoff_candidate`, `speaker_handoff_ready`,
  `speaker_handoff_block_reason` e contadores de frames/samples/falhas. Os
  endpoints `POST /api/audio/io-v2/speaker-handoff/enable|disable` ligam apenas
  a observacao; `speaker_handoff_active` permanece `false`, entao Playback v2
  ainda nao toca HAL/speaker. Validacao local: contrato Voice Audio v2 passou e
  `idf.py build` concluiu sem warnings.
- Validacao fisica apos flash do hash `afd885c`: baseline corrigido retornou
  `speaker_handoff_dry_run_enabled=false` e
  `speaker_handoff_block_reason=DISABLED`; ao habilitar o dry-run, o estado
  iniciou em `NO_TX` e depois virou `candidate=true`, `ready=true`,
  `block_reason=NONE` apenas observando TX. Durante um turno real curto
  (`ww -> que horas sao`), o dry-run acumulou `speaker_handoff_frames=18725`,
  `speaker_handoff_samples=4793600`, `speaker_handoff_silence_frames=18288`,
  com 437 frames nao silenciosos no TX real, `speaker_handoff_failures=0`,
  `speaker_handoff_recoveries=0`, `dropped_frames=0`, `i2s_recoveries=0` e
  `speaker_handoff_active=false`. Playback v2 fechou fila SAY em zero
  (`received=437`, `played=437`, drops cumulativos antigos=66); Capture v2
  ficou dono real do TX upstream com zero drops. O Codec v2 estava em PCM16
  nesse teste, portanto a evidencia e N3 dry-run PCM16; repetir com Opus v2
  ativo antes de promover qualquer owner real.
- Incremento server-only fechado no hash `562ca9e`: o server passou a expor
  `GET /api/device/audio/io-v2`,
  `POST /api/device/audio/io-v2/speaker-handoff/enable|disable` e o CLI
  `noisebot_server debug io-v2 status|speaker-handoff-enable|speaker-handoff-disable`.
  Validacao local: `server/tests` com 185 testes verdes; validacao live pelo
  CLI confirmou baseline `DISABLED`, enable em `NO_TX` e rollback para
  `DISABLED`, sempre com `speaker_handoff_active=false`.
- Validacao Opus v2 do dry-run: server conectado em Opus
  (`/ai/status` com `audio.format=opus`, `codecs.opus=true`), `codec-v2 health`
  inicialmente limpo, dry-run ligado via CLI e turno real curto
  `ww -> que horas sao`. O turno `178` fechou com transcript bom, intent
  `local_time`, `tts_completed=true`, `tts_say_end_sent=true`,
  `voice_end_reason=silence`, 44 chunks Opus upstream e 383 chunks SAY. O
  Audio IO v2 reportou `speaker_handoff_active=false`, `ready=true`,
  `block_reason=NONE`, `speaker_handoff_frames=123866`,
  `speaker_handoff_samples=31709696`, `speaker_handoff_silence_frames=122883`,
  logo 983 frames nao silenciosos observados, `speaker_handoff_failures=0`,
  `speaker_handoff_recoveries=0`, `dropped_frames=0`, `i2s_recoveries=0`.
  Playback v2 terminou com `say_queue_count=0`; havia apenas 1 pacote egress
  Opus residual, drenado por `codec-v2 egress-drain`, e o health final voltou
  `healthy=true/status=ok`, fila egress zero e `opus_codec_error=0`. O dry-run
  foi desabilitado ao final e voltou a `DISABLED`.
- Proximo incremento N3 recomendado: desenhar o owner real controlado do
  speaker pelo contrato Audio IO v2, ainda default-off e sem remover o caminho
  legado do `audio_service`. O primeiro passo deve ser uma flag de owner
  bloqueada por gates (`speaker_handoff_ready`, zero drops/recoveries, Opus
  health ok e rollback imediato para `speaker-handoff-disable`).
- Incremento de arm/disarm do owner controlado fechado nos hashes `f2de809` e
  `f332619`: o firmware expoe
  `speaker_handoff_owner_requested`/`speaker_handoff_owner_ready` e os
  endpoints `POST /api/audio/io-v2/speaker-handoff/owner/arm|disarm`; o server
  espelha isso no CLI `noisebot_server debug io-v2
  speaker-handoff-owner-arm|speaker-handoff-owner-disarm`. O primeiro flash de
  `f2de809` revelou timeout no HTTP ao montar o JSON grande de `/api/audio/io-v2`;
  `f332619` moveu esse buffer para memoria estatica dedicada e o build voltou
  limpo. Validacao fisica apos flash de `f332619`: baseline respondeu
  `owner_requested=false`, `owner_ready=false`, `active=false`,
  `block_reason=DISABLED`; `owner-arm` respondeu sem derrubar HTTP, iniciou em
  `NO_TX`, e a leitura seguinte mostrou `owner_requested=true`,
  `owner_ready=true`, `active=false`, `speaker_handoff_ready=true`,
  `block_reason=NONE`, `speaker_handoff_frames=1368`, `failures=0`,
  `recoveries=0`, `dropped_frames=0` e `i2s_recoveries=0`. `owner-disarm`
  voltou para `DISABLED` com owner/dry-run desligados. Status: N3 pronta para
  fechar; o owner real do speaker fica para N4.

### N4 - Playback v2 Assume HAL/Speaker

Promover so depois de I/O v2 previsivel:

- `audio_playback_service_v2` deixa de ser apenas dono da fila e passa a escrever
  no caminho de speaker via contrato I/O v2.
- `audio_service.c` deixa de drenar SAY para o HAL quando a flag v2 estiver
  ativa.
- Aceite: resposta curta/longa, cancelamento, no-echo, fila SAY e rollback
  PCM16/Opus continuam verdes.

Incremento N4.1 iniciado:

- Playback v2 ganhou o gate explicito `speaker-owner arm|disarm`, exposto por
  `/api/audio/playback-v2/speaker-owner/arm|disarm` e pelo CLI
  `debug playback-v2 speaker-owner-arm|speaker-owner-disarm`.
- O gate ainda delega para `audio_io_service_v2_set_speaker_handoff_owner_requested()`
  e apenas espelha `speaker_owner_requested`, `speaker_owner_ready` e
  `speaker_owner_active` em `/api/audio/playback-v2`; Playback v2 ainda nao
  chama `audio_hal_*` nem escreve direto no HAL.
- Validacao local antes do flash: contrato focado Voice Audio v2 verde
  (`7 passed`), facade do server verde (`171 passed`), suite do server verde
  (`188 passed`) e `idf.py build` completo sem warnings.
- Validacao fisica apos flash de `f7e6ed1`: `debug playback-v2
  speaker-owner-arm --json` respondeu `ok=true`; o snapshot estabilizado de
  Playback v2 mostrou `speaker_owner_requested=true`,
  `speaker_owner_ready=true`, `speaker_owner_active=false`, fila zero e
  `ESP_OK`. Audio IO v2 confirmou `speaker_handoff_dry_run_enabled=true`,
  `speaker_handoff_owner_requested=true`, `speaker_handoff_owner_ready=true`,
  `speaker_handoff_ready=true`, `speaker_handoff_active=false`,
  `block_reason=NONE`, `speaker_handoff_frames=1435`, zero falhas, zero
  recoveries, zero drops e zero `i2s_recoveries`. `speaker-owner-disarm`
  voltou Playback v2 para false/false/false e Audio IO v2 para
  `block_reason=DISABLED`.
- Observacao operacional: apos flash/reboot, `codec-v2 health` ficou degradado
  enquanto o worker Opus ainda nao estava iniciado (`opus_codec_error=-1`);
  `debug codec-v2 transport-enable --json` reativou o worker e o health voltou
  `healthy=true`, `status=ok`, zero drops e `opus_codec_error=0`.

Incremento N4.2 iniciado:

- Audio IO v2 ganhou `audio_io_service_v2_speaker_handoff_note_playback_frame()`
  para marcar especificamente quando um frame nao silencioso de Playback v2/SAY
  passou pelo gate de owner armado com write `ESP_OK`.
- `audio_service.c` chama esse marcador somente no caminho `PLAY_BRIDGE_SAY`,
  logo depois do write no speaker e da anotacao generica de TX. Assim
  `speaker_handoff_active=true` passa a significar "Playback v2/SAY entrou no
  gate" e nao apenas "houve TX qualquer".
- Ainda nao ha chamada `audio_hal_*` dentro de Playback v2; o HAL continua
  sendo escrito por `audio_service.c`, com Audio IO v2 como barreira/telemetria.
- Validacao local antes do flash: contrato focado Voice Audio v2 verde
  (`7 passed`) e `idf.py build` completo sem warnings. Proximo gate fisico:
  flashar, armar `debug playback-v2 speaker-owner-arm --json`, executar uma
  resposta real curta e confirmar `speaker_owner_active=true` /
  `speaker_handoff_active=true`, zero falhas, zero recoveries, zero drops e
  rollback por `speaker-owner-disarm`.
- Validacao fisica apos flash de `2bfb936`: apos `codec-v2 transport-enable`,
  o gate armado ficou `speaker_handoff_owner_requested=true`,
  `speaker_handoff_owner_ready=true`, `speaker_handoff_active=false` e
  `block_reason=NONE`. No turno real curto `ww -> que horas sao?`, Playback v2
  tocou 373/373 chunks SAY, fila final zero e zero drops; Playback v2 reportou
  `speaker_owner_active=true` e Audio IO v2 reportou
  `speaker_handoff_active=true`, `speaker_handoff_ready=true`,
  `block_reason=NONE`, 9219 frames de handoff, 8846 silenciosos, zero falhas,
  zero recoveries, zero `dropped_frames` e zero `i2s_recoveries`.
  `speaker-owner-disarm` voltou o gate para `DISABLED`. Havia 1 pacote egress
  Opus residual; `codec-v2 egress-drain` drenou e o health final voltou
  `healthy=true`, `status=ok`, zero drops e `opus_codec_error=0`.

Incremento N4.3 iniciado:

- Playback v2 ganhou `audio_playback_service_v2_speaker_next_frame()` como
  provider explicito de frame SAY para speaker. A funcao drena a fila SAY v2
  para um chunk fornecido pelo caller, evitando copia extra no loop de speaker.
- `audio_service.c` passou a pedir o proximo frame de speaker para Playback v2
  no estado `PLAY_BRIDGE_SAY`, em vez de chamar diretamente o dequeue da fila.
  `audio_service` ainda aplica volume, escreve no HAL e registra o resultado
  no Audio IO v2.
- Isso ainda nao transfere o HAL para Playback v2; e o passo intermediario para
  separar selecao/fornecimento de frame do write fisico.
- Gate fisico esperado: apos flash, resposta real curta deve manter 0 drops,
  fila SAY final zero, `speaker_handoff_active=true` quando armado, e rollback
  por `speaker-owner-disarm`.
- Primeira validacao fisica apos `2e50c1b` acendeu o active-shadow, mas nao
  fechou o gate: o turno mostrou `speaker_owner_active=true` e
  `speaker_handoff_active=true`, porem Playback v2 acumulou 907 chunks
  recebidos, 906 tocados, 30 drops e 1 cancelamento. O firmware ficou sem
  falhas de I/O (`dropped_frames=0`, `i2s_recoveries=0`, codec health ok), mas
  N4.3 exigiu ajuste para remover a copia extra do provider antes de repetir o
  gate.
- Validacao fisica apos ajuste `5dfce7a`: depois de reativar Opus e armar
  `speaker-owner`, o gate ficou `owner_ready=true`, `active=false`,
  `block_reason=NONE`. No turno real curto, Playback v2 saiu de 262/262 para
  1017/1017 chunks SAY, delta +755 recebidos/tocados, fila final zero,
  zero drops, zero cancelamentos e `speaker_owner_active=true`. Audio IO v2
  confirmou `speaker_handoff_active=true`, `speaker_handoff_ready=true`,
  `block_reason=NONE`, zero falhas, zero recoveries, zero `dropped_frames` e
  zero `i2s_recoveries`; `codec-v2 health` ficou `healthy=true/status=ok`.
  `speaker-owner-disarm` voltou Playback v2 para false/false/false e Audio IO
  para `DISABLED`.

Incremento N4.4 iniciado:

- Playback v2 agora prepara o frame SAY destinado ao speaker dentro de
  `audio_playback_service_v2_speaker_next_frame()`: alem de drenar a fila, ele
  limita o tamanho do chunk, aplica volume percentual com clamp e devolve PCM ja
  pronto para o write fisico.
- `/api/audio/playback-v2` expoe `speaker_frames_prepared`,
  `speaker_samples_prepared`, `speaker_last_samples` e `speaker_last_volume`
  para validar se o preparo do speaker esta acompanhando `say_chunks_played`.
- `audio_service.c` continua o unico ponto que chama `audio_hal_spk_write()` no
  caminho SAY; ele apenas solicita o frame preparado ao Playback v2, escreve no
  HAL e registra o resultado no Audio IO v2.
- Validacao local antes do flash: contrato focado Voice Audio v2 verde
  (`7 passed`), facade do server verde (`171 passed`), `git diff --check`
  limpo e `idf.py build` completo sem warnings. Gate fisico esperado: apos
  flash, armar `speaker-owner`, executar resposta real curta e confirmar
  `speaker_frames_prepared` crescendo no mesmo delta de `say_chunks_played`,
  zero drops/cancelamentos, `speaker_handoff_active=true`, Codec v2 saudavel e
  rollback por `speaker-owner-disarm`.
- Validacao fisica apos flash: baseline limpo, Codec v2 reativado por
  `transport-enable`, gate estabilizado com `speaker_owner_ready=true` e Audio
  IO v2 em `block_reason=NONE`. No turno curto real, Playback v2 reportou
  `speaker_frames_prepared=380`, `speaker_samples_prepared=97280`,
  `speaker_last_samples=256`, `speaker_last_volume=80`, `say_chunks_received=380`
  e `say_chunks_played=380`, com fila final zero, zero drops, zero drops durante
  listening, zero cancelamentos e `speaker_owner_active=true`. Audio IO v2
  confirmou `speaker_handoff_active=true`, `speaker_handoff_ready=true`,
  `speaker_handoff_frames=3230`, 380 frames nao silenciosos, zero falhas,
  zero recoveries, zero `dropped_frames` e zero `i2s_recoveries`; Codec v2 ficou
  `healthy=true/status=ok`. O rollback `speaker-owner-disarm` voltou Playback v2
  para false/false/false e Audio IO v2 para `DISABLED`.

Incremento N4.5 iniciado:

- Playback v2 ganhou `audio_playback_service_v2_speaker_commit_frame()` para
  fechar o contrato do frame SAY apos o write fisico. O `audio_service` ainda
  chama `audio_hal_spk_write()`, mas passa o resultado para Playback v2, que
  registra o commit e aciona o marcador de handoff no Audio IO v2.
- `/api/audio/playback-v2` expoe `speaker_frames_committed`,
  `speaker_samples_committed`, `speaker_commit_failures`,
  `speaker_last_commit_samples` e `speaker_last_commit_result`.
- `audio_service.c` deixou de chamar diretamente
  `audio_io_service_v2_speaker_handoff_note_playback_frame()` no caminho SAY,
  reduzindo o acoplamento do loop legado com o Audio IO v2 sem colocar HAL
  dentro do Playback v2.
- Validacao local antes do flash: contrato focado Voice Audio v2 verde
  (`7 passed`), facade do server verde (`171 passed`), `git diff --check`
  limpo e `idf.py build` completo sem warnings. Gate fisico esperado: apos
  flash, armar `speaker-owner`, executar resposta real curta e confirmar
  `speaker_frames_committed` acompanhando `speaker_frames_prepared` e
  `say_chunks_played`, `speaker_commit_failures=0`, zero drops/cancelamentos,
  `speaker_handoff_active=true`, Codec v2 saudavel e rollback por
  `speaker-owner-disarm`.
- Validacao fisica apos flash: baseline limpo, Codec v2 reativado por
  `transport-enable`, gate estabilizado com `speaker_owner_ready=true` e Audio
  IO v2 em `block_reason=NONE`. No turno curto real, Playback v2 reportou
  `speaker_frames_prepared=388`, `speaker_frames_committed=388`,
  `speaker_samples_prepared=99328`, `speaker_samples_committed=99328`,
  `speaker_commit_failures=0`, `speaker_last_commit_samples=256`,
  `speaker_last_commit_result=ESP_OK`, `say_chunks_received=388` e
  `say_chunks_played=388`, com fila final zero, zero drops, zero drops durante
  listening, zero cancelamentos e `speaker_owner_active=true`. Audio IO v2
  confirmou `speaker_handoff_active=true`, `speaker_handoff_ready=true`,
  `speaker_handoff_frames=3495`, 388 frames nao silenciosos, zero falhas,
  zero recoveries, zero `dropped_frames` e zero `i2s_recoveries`; Codec v2 ficou
  `healthy=true/status=ok`. O rollback `speaker-owner-disarm` voltou Playback v2
  para false/false/false e Audio IO v2 para `DISABLED`.

Incremento N4.6 iniciado:

- Playback v2 ganhou `audio_playback_service_v2_speaker_note_empty()` para
  assumir a janela de fila SAY vazia antes de encerrar a fala. O `audio_service`
  ainda muda `PLAY_BRIDGE_SAY` para `PLAY_IDLE` e emite `PLAYBACK_END`, mas
  passa a perguntar ao Playback v2 quando a janela de idle venceu.
- `/api/audio/playback-v2` expoe `speaker_empty_polls`, `speaker_empty_ms` e
  `speaker_idle_end_count` para validar a politica de jitter/idle do SAY.
- O preparo de frame, commit de write e agora a decisao de idle do SAY ficam no
  Playback v2; o HAL fisico continua exclusivamente em `audio_service.c`.
- Validacao local antes do flash: contrato focado Voice Audio v2 verde
  (`7 passed`), facade do server verde (`171 passed`), `git diff --check`
  limpo e `idf.py build` completo sem warnings. Gate fisico esperado: apos
  flash, armar `speaker-owner`, executar resposta real curta e confirmar
  prepared/committed/played em lockstep, `speaker_commit_failures=0`,
  `speaker_empty_polls` crescendo apenas no final/jitter da fala,
  `speaker_idle_end_count>=1`, fila final zero, zero drops/cancelamentos, Audio
  IO v2 sem falhas/recoveries e rollback por `speaker-owner-disarm`.
- Validacao fisica apos flash: baseline limpo, Codec v2 reativado por
  `transport-enable`, gate estabilizado com `speaker_owner_ready=true` e Audio
  IO v2 em `block_reason=NONE`. A primeira rodada real confirmou o novo
  contrato de idle (`speaker_empty_polls=87`, `speaker_empty_ms=0`,
  `speaker_idle_end_count=1`) e manteve prepared/committed/played em lockstep
  (`387/387/387`), `speaker_commit_failures=0`, fila final zero, Audio IO sem
  falhas/recoveries/dropped/I2S recoveries e Codec v2 saudavel, mas acumulou
  1 drop SAY. Repeticao com o gate rearmado fechou por delta: Playback v2 foi de
  387 para 775 prepared/committed/played (+388), `say_chunks_dropped` permaneceu
  em 1 (zero drops novos), `speaker_idle_end_count` subiu de 1 para 2,
  `speaker_empty_polls` foi para 163, `speaker_commit_failures=0` e
  `speaker_last_commit_result=ESP_OK`. Audio IO v2 manteve
  `speaker_handoff_active=true`, `block_reason=NONE`, zero falhas, zero
  recoveries, zero `dropped_frames` e zero `i2s_recoveries`; Codec v2 ficou
  `healthy=true/status=ok` apos drenar 1 pacote egress residual. O rollback
  `speaker-owner-disarm` voltou Playback v2 para false/false/false e Audio IO v2
  para `DISABLED`.

Incremento N4.7 iniciado:

- Playback v2 ganhou `audio_playback_service_v2_speaker_write_next_frame()`,
  que puxa/prepara o frame SAY, chama um callback de escrita fornecido pelo
  `audio_service`, registra o commit e devolve `sample_count`/resultado ao loop
  legado.
- O `audio_service` nao chama mais `audio_hal_spk_write()` diretamente no bloco
  `PLAY_BRIDGE_SAY`; ele fornece `audio_service_playback_v2_write_speaker()`
  como callback. Assim o HAL fisico continua fora do Playback v2, mas a
  orquestracao do write SAY passa para o contrato v2.
- `/api/audio/playback-v2` agora expoe `speaker_write_requests`,
  `speaker_write_samples`, `speaker_write_failures`,
  `speaker_last_write_samples` e `speaker_last_write_result` para comparar
  prepared/write/committed/played em lockstep.
- Validacao local antes do flash: contrato focado Voice Audio v2 verde
  (`7 passed`), facade do server verde (`178 passed`) e `idf.py build`
  completo sem warnings. Gate fisico esperado: apos flash, armar
  `speaker-owner`, executar resposta real curta e confirmar
  `speaker_write_requests` crescendo no mesmo delta de `speaker_frames_prepared`,
  `speaker_frames_committed` e `say_chunks_played`, com zero falhas de write,
  zero drops/cancelamentos, Audio IO v2 sem recoveries/drops, Codec v2
  saudavel e rollback por `speaker-owner-disarm`.
- Refino server-only de pacing SAY apos validacao fisica: a rampa inicial
  conservadora do `OutputScheduler` foi mantida como opt-in
  (`NOISEBOT_TTS_STARTUP_CHUNKS`), mas desabilitada por default
  (`NOISEBOT_TTS_STARTUP_CHUNKS=0`) porque aumentou a latencia percebida da
  fala. O envio nominal voltou a acompanhar o chunk real de 16 ms por chunk.
  Isso nao altera firmware,
  HAL, wake, captura ou codec. Validacao anterior em hardware apos restart do
  server: baseline Playback v2
  `received=2400/played=2398/dropped=15`; turno real `ww -> que horas sao?`
  enviou 411 chunks TTS e fechou em `received=2811/played=2809/dropped=15`,
  fila final zero, `speaker_write_failures=0`, `speaker_commit_failures=0`,
  Audio IO v2 sem `dropped_frames`/`i2s_recoveries` e Codec v2 sem drops.

Incremento N4.8 iniciado:

- O CLI `debug playback-v2 delta` virou gate operacional agregado: alem dos
  deltas SAY, ele agora coleta Playback v2, Audio IO v2 e Codec v2 antes/depois
  do turno e emite `status=ok|warn|fail`, `issues` e `warnings`.
- Falhas duras: fila SAY final nao vazia, novos drops SAY/listening, falhas de
  write/commit do speaker, `dropped_frames`/`i2s_recoveries` ou falhas/recoveries
  de Audio IO v2, drops/erros de Codec v2 ou Codec v2 nao saudavel.
- Avisos nao bloqueantes atuais: `opus_egress_queue_count` residual no health do
  Codec v2 e `heap_internal_free_kb` abaixo de 16 KB. Esses avisos mantem
  `ok=true` quando nao ha falha funcional, mas deixam `status=warn`.
- Validacao local: teste focado do CLI Playback v2 verde (`3 passed`). O facade
  completo do server deve permanecer verde apos a limpeza dos comandos locais
  de brilho.
- Validacao viva sem prompt contra o firmware: `ok=true`, `status=warn`,
  `issues=[]`, deltas criticos zerados (`say_chunks_dropped=0`,
  `speaker_write_failures=0`, `speaker_commit_failures=0`,
  `dropped_frames=0`, `i2s_recoveries=0`, `packet_drops=0`,
  `opus_egress_packet_drops=0`, `opus_codec_error=0`) e fila SAY final zero.

### N5 - Reduzir audio_service.c

Ultima etapa, apos donos reais validados:

- `audio_service.c` vira ponte/compatibilidade e orquestracao legada minima.
- Remover decisao duplicada apenas quando os servicos v2 ja tiverem gates e
  rollback equivalentes.
- Aceite: arquitetura em camadas preservada, nenhum comportamento v1 quebra, e
  o caminho default de voz em hardware passa por Capture v2, Activity v2,
  Audio IO v2 e Playback v2.

Incremento N5.1 iniciado:

- O bloco `PLAY_BRIDGE_SAY` do loop principal de `audio_service.c` foi isolado
  em `audio_service_play_bridge_say_chunk()`. O helper continua chamando
  `audio_playback_service_v2_speaker_write_next_frame()` e usando o callback
  local `audio_service_playback_v2_write_speaker()` para manter `audio_service`
  como unico ponto que escreve fisicamente no HAL.
- Esta etapa nao muda flags, wake, VAD, captura, Codec v2, PCM16/Opus ou dono
  real do HAL; apenas reduz a superficie do loop legado e deixa a proxima
  remocao de decisao duplicada mais localizada.
- Gate esperado apos build/flash: `debug playback-v2 delta` em turno real curto
  sem `issues`, fila SAY final zero, `speaker_write_failures=0`,
  `speaker_commit_failures=0`, Audio IO v2 sem drops/recoveries e Codec v2 sem
  drops/erros. Warnings de heap/egress residual continuam nao bloqueantes se os
  deltas criticos permanecerem zerados.
- Validacao fisica apos flash: baseline sem prompt retornou `ok=true`,
  `status=ok`, zero warnings e deltas zerados. Com server local visivel e gate
  temporizado `debug playback-v2 delta --wait-s 35 --json`, turno real curto
  fechou com `ok=true`, `status=warn`, `issues=[]`, fila SAY final zero,
  `say_chunks_received=386`, `say_chunks_played=386`, zero drops/cancelamentos,
  `speaker_write_requests=386`, `speaker_write_failures=0`,
  `speaker_frames_committed=386`, `speaker_commit_failures=0`, Audio IO v2 sem
  `dropped_frames`/`i2s_recoveries`/handoff failures, Codec v2 `healthy=true`
  com zero drops/erros. O unico warning foi `heap_internal_free_kb baixo: 11`.

Incremento N5.2 iniciado:

- A politica de idle/end do SAY deixou de ficar parametrizada no
  `audio_service.c`: Playback v2 agora expoe
  `audio_playback_service_v2_speaker_should_end_idle()`, com os limiares internos
  `NB_AUDIO_PLAYBACK_V2_CHUNK_MS=16` e
  `NB_AUDIO_PLAYBACK_V2_SAY_IDLE_END_MS=1200`.
- O helper parametrizado de empty/idle ficou privado dentro de Playback v2, e o
  estado legado `bridge_say_empty_ms` foi removido de `audio_service.c`.
- `audio_service.c` ainda aplica a transicao `PLAY_BRIDGE_SAY -> PLAY_IDLE` e
  emite `NB_AUDIO_EVT_PLAYBACK_END`, porque o event callback e o estado legado
  ainda moram nele. O dono fisico do HAL continua inalterado.
- Gate esperado: repetir o mesmo `playback-v2 delta --wait-s` com resposta real
  curta e confirmar fila SAY final zero, zero drops/cancelamentos, zero falhas
  de write/commit, Audio IO v2 sem drops/recoveries e Codec v2 sem drops/erros.
- Validacao fisica apos flash: baseline sem prompt retornou `ok=true`,
  `status=ok`, `issues=[]`, `warnings=[]` e deltas zerados. Turno real curto
  com `debug playback-v2 delta --wait-s 35 --json` fechou com `ok=true`,
  `status=warn`, `issues=[]`, fila SAY final zero,
  `say_chunks_received=368`, `say_chunks_played=368`, zero
  drops/cancelamentos, `speaker_write_requests=368`,
  `speaker_write_failures=0`, `speaker_frames_committed=368`,
  `speaker_commit_failures=0`, Audio IO v2 sem `dropped_frames`,
  `i2s_recoveries` ou handoff failures, Codec v2 `healthy=true` com zero
  drops/erros. O unico warning foi `heap_internal_free_kb baixo: 11`.

Incremento N5.3 iniciado:

- O contrato de ciclo SAY ficou mais semantico em Playback v2: `audio_service`
  passa a chamar `audio_playback_service_v2_say_accept()`,
  `audio_playback_service_v2_say_cancel_active()`,
  `audio_playback_service_v2_say_drop_listening()` e
  `audio_playback_service_v2_say_end_idle()`.
- As APIs publicas antigas de detalhe (`say_enqueue`, `say_dequeue`,
  `note_say_*` e `say_cancel`) foram removidas do header; `say_dequeue` ficou
  privado dentro de Playback v2, e a drenagem externa continua sendo
  `audio_playback_service_v2_speaker_next_frame()` /
  `audio_playback_service_v2_speaker_write_next_frame()`.
- `audio_service.c` ainda decide quando entrar/sair de `PLAY_BRIDGE_SAY` e
  emitir eventos de playback, mas nao manipula mais diretamente os contadores e
  a fila SAY por APIs de baixo nivel.
- Gate esperado: build limpo, flash, baseline `playback-v2 delta --no-prompt`
  limpo e turno real curto com zero drops, zero falhas de speaker write/commit,
  fila SAY final zero e Codec/Audio IO v2 sem drops/recoveries.
- Validacao fisica apos flash: baseline sem prompt retornou `ok=true`,
  `issues=[]`, fila SAY zero e deltas criticos zerados, com warnings conhecidos
  de `opus_egress_queue_count=1` e `heap_internal_free_kb baixo: 11`. A primeira
  janela real nao gerou SAY novo; repeticao com `--wait-s 50` capturou resposta
  real com `ok=true`, `status=warn`, `issues=[]`, fila SAY final zero,
  `say_chunks_received=409`, `say_chunks_played=409`, zero drops/cancelamentos,
  `speaker_write_requests=409`, `speaker_write_failures=0`,
  `speaker_frames_committed=409`, `speaker_commit_failures=0`, Audio IO v2 sem
  `dropped_frames`/`i2s_recoveries`/handoff failures e Codec v2 `healthy=true`
  com zero drops/erros. O unico warning final foi `heap_internal_free_kb baixo:
  11`.

Incremento N5.4 iniciado:

- O estado duplicado `bridge_say_playing` foi removido de `audio_service.c`.
  Dentro do servico legado, SAY ativo agora e representado somente por
  `play_state == PLAY_BRIDGE_SAY`.
- Os contextos de Activity v2, `audio_is_playing()`, `audio_service_is_busy()`,
  stop/cancel e entrada de chunks SAY passaram a usar essa fonte unica.
- O `audio_service` ainda aplica a transicao de estado e emite
  `NB_AUDIO_EVT_PLAYBACK_START/END`; Playback v2 continua dono dos contadores,
  fila, idle/end e ciclo semantico SAY. O HAL fisico segue inalterado.
- Gate esperado: build limpo, flash, baseline `playback-v2 delta --no-prompt`
  sem issues e turno real curto com fila SAY final zero, zero drops, zero
  cancelamentos, zero falhas de speaker write/commit, Audio IO v2 sem
  drops/recoveries e Codec v2 sem drops/erros.
- Validacao fisica apos flash: baseline sem prompt retornou `ok=true`,
  `status=warn`, `issues=[]`, fila SAY zero e deltas criticos zerados; o unico
  warning foi `heap_internal_free_kb baixo: 12`. A primeira janela real nao
  gerou SAY novo; repeticao com `--wait-s 70` capturou resposta real com
  `ok=true`, `status=warn`, `issues=[]`, fila SAY final zero,
  `say_chunks_received=127`, `say_chunks_played=127`, zero
  drops/cancelamentos, `speaker_write_requests=127`,
  `speaker_write_failures=0`, `speaker_frames_committed=127`,
  `speaker_commit_failures=0`, Audio IO v2 sem
  `dropped_frames`/`i2s_recoveries`/handoff failures e Codec v2 `healthy=true`
  com zero drops/erros. O unico warning final foi `heap_internal_free_kb baixo:
  11`.

Incremento N5.5 iniciado:

- Os contadores locais duplicados de SAY em `audio_service.c`
  (`s_bridge_say_*_count`) foram removidos. A fonte oficial de telemetria de
  received/played/dropped/cancelled passa a ser somente Playback v2.
- Os logs periodicos baseados nesses contadores legados tambem foram removidos;
  o `audio_service` continua apenas iniciando o modo `PLAY_BRIDGE_SAY`,
  rearmando wake e entregando chunks para `audio_playback_service_v2_say_accept()`.
- Esta etapa nao muda HAL fisico, fila SAY, wake, captura, Activity v2, Codec v2
  ou rollback. Gate esperado: build limpo, flash, baseline sem issues e turno
  real curto com fila SAY final zero, received=played, zero drops/cancelamentos,
  zero falhas de speaker write/commit, Audio IO v2 sem drops/recoveries e Codec
  v2 sem drops/erros.
- Validacao fisica apos flash: baseline sem prompt retornou `ok=true`,
  `status=warn`, `issues=[]`, fila SAY zero e deltas criticos zerados, com
  warnings conhecidos de `opus_egress_queue_count=1` e
  `heap_internal_free_kb baixo: 11`. A janela real seguinte capturou resposta
  nova com `ok=true`, `status=warn`, `issues=[]`, fila SAY final zero,
  `say_chunks_received=243`, `say_chunks_played=243`, zero
  drops/cancelamentos, `speaker_write_requests=243`,
  `speaker_write_failures=0`, `speaker_frames_committed=243`,
  `speaker_commit_failures=0`, Audio IO v2 sem
  `dropped_frames`/`i2s_recoveries`/handoff failures e Codec v2 `healthy=true`,
  `status=ok`, sem warnings e com zero drops/erros. O unico warning agregado
  final foi `heap_internal_free_kb baixo: 11`.

Incremento N5.6 iniciado:

- A API publica `audio_playback_service_v2_say_pending_count()` foi removida do
  contrato Playback v2 porque ficou sem chamadores apos a N5.5.
- A telemetria de profundidade da fila continua disponivel por
  `/api/audio/playback-v2` via `say_queue_count`, alimentada internamente pelo
  proprio Playback v2.
- Esta etapa nao muda runtime, fila SAY, HAL fisico, wake, captura, Activity v2,
  Codec v2 ou rollback; apenas reduz a superficie publica exposta para
  `audio_service.c` e demais componentes.
- Validacao fisica apos flash: baseline sem prompt retornou `ok=true`,
  `status=warn`, `issues=[]`, fila SAY final zero, deltas criticos zerados,
  Codec v2 `healthy=true/status=ok`, sem drops/erros; o unico warning foi
  `heap_internal_free_kb baixo: 11`. A primeira janela real capturou SAY, mas
  terminou com cauda de fila (`say_queue_count=4`); baseline imediato confirmou
  drenagem para `say_queue_count=0` e `received=played=361`, sem drops/falhas.
  Repeticao com `--wait-s 120` fechou o gate real com `ok=true`, `status=warn`,
  `issues=[]`, fila SAY final zero, `say_chunks_received=105`,
  `say_chunks_played=105`, zero drops/cancelamentos,
  `speaker_write_requests=105`, `speaker_write_failures=0`,
  `speaker_frames_committed=105`, `speaker_commit_failures=0`, Audio IO v2 sem
  `dropped_frames`/`i2s_recoveries`/handoff failures e Codec v2 `healthy=true`,
  `status=ok`, sem warnings e com zero drops/erros.

Incremento N5.7 iniciado:

- Os helpers de detalhe `audio_playback_service_v2_speaker_next_frame()` e
  `audio_playback_service_v2_speaker_commit_frame()` deixaram de ser API
  publica. Eles ficaram privados dentro de Playback v2 como
  `playback_v2_speaker_next_frame()` e `playback_v2_speaker_commit_frame()`.
- O contrato publico mantem apenas
  `audio_playback_service_v2_speaker_write_next_frame()`, que puxa/prepara o
  frame SAY, chama o callback de escrita fisica fornecido por `audio_service` e
  registra commit/telemetria dentro de Playback v2.
- Esta etapa nao muda HAL fisico, fila SAY, wake, captura, Activity v2, Codec v2
  ou rollback; ela reduz a superficie publica e atualiza o teste de contrato
  para exigir o desenho semantico atual.
- Validacao fisica apos flash: baseline sem prompt retornou `ok=true`,
  `status=ok`, `issues=[]`, `warnings=[]`, fila SAY zero e deltas criticos
  zerados. A primeira janela real capturou resposta completa do server
  (`tts_completed=true`, `tts_say_end_sent=true`, `tts_chunks_sent=243`), mas
  Playback v2 registrou `say_chunks_dropped=36`; fila final ficou zero,
  speaker write/commit nao falharam e Audio IO/Codec v2 ficaram sem drops/erros.
  Como gate, essa rodada foi tratada como falha de escoamento SAY e nao como
  validacao. Repeticao com comando curto e `--wait-s 120` fechou limpo:
  `ok=true`, `status=warn`, `issues=[]`, fila SAY final zero,
  `say_chunks_received=367`, `say_chunks_played=367`, zero
  drops/cancelamentos, `speaker_write_requests=367`,
  `speaker_write_failures=0`, `speaker_frames_committed=367`,
  `speaker_commit_failures=0`, Audio IO v2 sem
  `dropped_frames`/`i2s_recoveries`/handoff failures e Codec v2 sem drops/erros.
  Os warnings finais foram os conhecidos `opus_egress_queue_count=1` e
  `heap_internal_free_kb baixo: 11`.

Incremento N5.8 iniciado:

- O tipo interno `nb_audio_playback_v2_say_chunk_t` deixou de ser exposto no
  header publico de Playback v2. Ele agora fica privado em
  `audio_playback_service_v2.c`, porque nenhuma API publica recebe ou devolve
  chunks SAY diretamente apos N5.7.
- O contrato publico continua aceitando PCM por
  `audio_playback_service_v2_say_accept()` e escrevendo speaker por
  `audio_playback_service_v2_speaker_write_next_frame()`.
- Esta etapa nao muda fila SAY, HAL fisico, wake, captura, Activity v2, Codec v2
  ou rollback; apenas reduz a superficie de tipos visiveis e atualiza o teste de
  contrato para proteger esse encapsulamento.
- Validacao fisica apos flash: baseline sem prompt retornou `ok=true`,
  `status=ok`, `issues=[]`, `warnings=[]`, fila SAY zero, deltas criticos
  zerados e Codec v2 `healthy=true/status=ok`. A janela real com comando curto
  e `--wait-s 120` fechou limpa: `ok=true`, `status=warn`, `issues=[]`, fila
  SAY final zero, `say_chunks_received=376`, `say_chunks_played=376`, zero
  drops/cancelamentos, `speaker_write_requests=376`,
  `speaker_write_failures=0`, `speaker_frames_committed=376`,
  `speaker_commit_failures=0`, Audio IO v2 sem
  `dropped_frames`/`i2s_recoveries`/handoff failures e Codec v2
  `healthy=true/status=ok`, sem warnings e com zero drops/erros. O unico
  warning agregado final foi `heap_internal_free_kb baixo: 11`.

Incremento N5.9 iniciado:

- As constantes internas de Playback v2 (`NB_AUDIO_PLAYBACK_V2_QUEUE_PACKETS`,
  `NB_AUDIO_PLAYBACK_V2_CHUNK_SAMPLES`, `NB_AUDIO_PLAYBACK_V2_CHUNK_MS`,
  `NB_AUDIO_PLAYBACK_V2_SAY_IDLE_END_MS`, `NB_AUDIO_PLAYBACK_V2_SAMPLE_RATE_HZ`
  e `NB_AUDIO_PLAYBACK_V2_PROBE_HZ`) deixaram de ser expostas no header publico.
- Elas agora ficam privadas em `audio_playback_service_v2.c`; a observabilidade
  publica continua via `/api/audio/playback-v2`, especialmente
  `say_queue_depth`, `say_queue_count`, `speaker_*` e `say_*`.
- Esta etapa nao muda fila SAY, HAL fisico, wake, captura, Activity v2, Codec v2
  ou rollback; apenas reduz a superficie de constantes visiveis e atualiza o
  teste de contrato para proteger esse encapsulamento.

Validacao N5.9 pos-flash:

- Baseline quieto em `/api/audio/playback-v2`: `ok=true`, `status=ok`,
  `issues=[]`, fila SAY vazia, zero drops/cancelamentos/falhas de speaker,
  Audio IO sem `dropped_frames`/`i2s_recoveries` e Codec v2 healthy.
- Resposta real curta: `say_chunks_received=379`, `say_chunks_played=379`,
  `say_chunks_dropped=0`, `speaker_write_failures=0`,
  `speaker_commit_failures=0`, `queue_empty=true` e `normal_path_clean=true`.
- Audio IO continuou sem `dropped_frames`, `i2s_recoveries`,
  `speaker_handoff_failures` ou `speaker_handoff_recoveries`.
- Warnings nao bloqueantes observados no agregado: `opus_egress_queue_count=1`
  e `heap_internal_free_kb baixo: 11`.

Incremento N5.10 iniciado:

- A politica de SAY recebido durante listening ficou mais encapsulada em
  Playback v2: `audio_playback_service_v2_say_drop_listening()` agora tambem
  limpa a fila SAY pendente e contabiliza cancelamento dos chunks antigos.
- `audio_service_bridge_say_chunk()` continua decidindo o estado legado
  (`PLAY_IDLE` durante escuta ou `PLAY_BRIDGE_SAY` no inicio da fala), mas nao
  combina mais manualmente `say_cancel_active()` com `say_drop_listening()` para
  esse caso.
- Esta etapa nao muda HAL fisico, wake, VAD, Capture v2, Activity v2, Codec v2,
  Opus/PCM16 ou policy; apenas move a semantica de descarte/cancelamento de SAY
  antigo durante listening para o dono da fila.

Validacao N5.10 pos-flash:

- Usuario confirmou build/flash/teste feitos.
- Gate esperado para aceitar a etapa: `playback-v2 delta` em turno real curto
  com fila SAY final zero, zero drops no caminho normal, zero falhas de
  speaker write/commit, Audio IO sem drops/recoveries e Codec v2 saudavel.

Incremento N5.11 iniciado:

- O limite de tamanho do chunk SAY deixou de ser aplicado em
  `audio_service_bridge_say_chunk()`.
- O clamp permanece dentro de `audio_playback_service_v2_say_accept()`, usando
  o limite interno `NB_AUDIO_PLAYBACK_V2_CHUNK_SAMPLES`. Assim o dono da fila
  tambem e dono da normalizacao do frame recebido.
- Esta etapa nao muda fila SAY, HAL fisico, wake, VAD, Capture v2, Activity v2,
  Codec v2, Opus/PCM16 ou policy; apenas remove uma decisao duplicada do
  `audio_service.c`.

Incremento N5.12 iniciado:

- `audio_service_bridge_say_chunk()` passou a iniciar `PLAY_BRIDGE_SAY` e emitir
  `NB_AUDIO_EVT_PLAYBACK_START` somente depois que
  `audio_playback_service_v2_say_accept()` aceita o primeiro chunk.
- Se a fila SAY estiver cheia ou indisponivel, Playback v2 contabiliza o erro
  pelo proprio `say_accept()` e o `audio_service` nao gera start/evento de
  playback sem audio enfileirado.
- O estado legado, wake rearm e callback de evento continuam em
  `audio_service.c`; Playback v2 continua dono da fila e dos contadores.
- Esta etapa nao muda HAL fisico, wake, VAD, Capture v2, Activity v2, Codec v2,
  Opus/PCM16 ou policy; apenas condiciona o start legado ao aceite real do dono
  da fila SAY.

Validacao N5.12 pos-flash:

- Baseline quieto em `/api/audio/playback-v2`: `ok=true`, `status=ok`,
  `issues=[]`, sem warnings, fila SAY vazia, zero drops/cancelamentos/falhas de
  speaker, Audio IO sem `dropped_frames`/`i2s_recoveries` e Codec v2 healthy.
- Resposta real curta: `say_chunks_received=521`, `say_chunks_played=521`,
  `say_chunks_dropped=0`, `speaker_write_failures=0`,
  `speaker_commit_failures=0`, `queue_empty=true` e `normal_path_clean=true`.
- Audio IO continuou sem `dropped_frames`, `i2s_recoveries`,
  `speaker_handoff_failures` ou `speaker_handoff_recoveries`.
- Warnings nao bloqueantes no agregado real: `opus_egress_queue_count=1` e
  `heap_internal_free_kb baixo: 11`.

Fechamento tecnico N5:

- N5 esta consolidada como reducao segura do `audio_service.c` para o caminho
  SAY/Playback v2 sem trocar o dono fisico do HAL.
- Playback v2 e dono da fila SAY, contadores `say_*`, cancel/drop, clamp do
  chunk, preparo/commit do frame de speaker, orquestracao do write por callback,
  politica de fila vazia e detalhes internos/constantes privadas.
- `audio_service.c` ainda mantem de forma intencional: estado legado
  `PLAY_BRIDGE_SAY`, callbacks `NB_AUDIO_EVT_PLAYBACK_START/END`,
  `wake_service_rearm()`, transicao final para `PLAY_IDLE` e a chamada fisica
  `audio_hal_spk_write()` pelo callback.
- Nao promover Playback v2 para HAL owner direto nesta fase. O proximo salto
  estrutural precisa ser uma fase nova/gate proprio para speaker/HAL ownership,
  com rollback explicito e validacao de barge/no-echo/Playback delta.

### N6 - Playback v2 speaker/HAL ownership controlado

Objetivo: preparar a troca real de ownership do speaker/HAL para Playback v2,
sem pular direto para `audio_hal_*` dentro do Playback v2. A fase deve comecar
por baseline e dry-run; owner real so entra por flag/gate proprio e com rollback
imediato para o callback atual do `audio_service`.

Incremento N6.0 iniciado:

- Baseline/gates congelados antes de qualquer mudanca de HAL owner.
- `codec-v2 health` ficou `healthy=true`, `status=ok`, zero drops, fila egress
  zero e worker `running` apos drenar 1 pacote residual.
- `io-v2 status` ficou `ok=true`, `rx_owner_active=true`,
  `rx_dispatch_last_consumers=8`, `tx_owner_observed=true`,
  `tx_owner_last_result=ESP_OK`, `dropped_frames=0`, `i2s_recoveries=0`,
  handoff de speaker desarmado (`speaker_handoff_block_reason=DISABLED`) e heap
  interno/DMA reportado em 11 KB.
- Baseline quieto de `playback-v2 delta --no-prompt`: fila SAY vazia, zero
  deltas de SAY/speaker, `normal_path_clean=true`, Codec v2 `status=ok` e unico
  warning agregado `heap_internal_free_kb baixo: 11`.
- Resposta real curta no gate `playback-v2 delta --wait-s 120`: recebeu/tocou
  `372/372` chunks SAY, `say_chunks_dropped=0`, `speaker_write_failures=0`,
  `speaker_commit_failures=0`, `queue_empty=true`, Audio IO sem
  `dropped_frames`/`i2s_recoveries` e Codec v2 sem drops/erro. O unico warning
  agregado real foi `opus_egress_queue_count=1` + heap baixo; o egress residual
  foi drenado depois e voltou a zero.
- Proximo incremento N6.1 deve ser dry-run/status de readiness do HAL owner,
  ainda sem chamada direta de `audio_hal_*` por Playback v2.

Incremento N6.1 iniciado:

- `/api/audio/playback-v2` passa a espelhar o readiness completo de speaker/HAL
  ownership calculado no Audio IO v2: `speaker_owner_dry_run_enabled`,
  `speaker_owner_candidate`, `speaker_owner_handoff_ready`,
  `speaker_owner_block_reason`, frames/samples/silencio, falhas, recoveries e
  ultimo resultado.
- O CLI `debug playback-v2 status` tambem mostra esse bloco de readiness para
  que o gate de HAL owner possa ser visto pelo namespace de Playback v2.
- Esta etapa nao muda HAL fisico, nao adiciona chamada `audio_hal_*` em
  Playback v2, nao altera wake/VAD/Capture/Activity/Codec/Opus/PCM16 e nao
  muda o caminho real de audio. Ela apenas torna N6.1 auditavel antes de um
  dry-run operacional.

Validacao N6.1 pos-flash:

- `/api/audio/playback-v2` expos os novos campos de readiness. Em idle/desarmado:
  `speaker_owner_dry_run_enabled=false`, `speaker_owner_requested=false`,
  `speaker_owner_block_reason=DISABLED`, contadores de readiness zerados e
  `error=ESP_OK`.
- `codec-v2 health` inicial ficou `healthy=true/status=ok`, zero drops, fila
  egress zero e worker `running`; `io-v2 status` mostrou RX/TX observados,
  `rx_dispatch_last_consumers=8`, `tx_owner_last_result=ESP_OK` e zero
  `dropped_frames`/`i2s_recoveries`.
- Ao armar pelo namespace Playback v2 (`debug playback-v2 speaker-owner-arm`),
  o status ficou `speaker_owner_dry_run_enabled=true`,
  `speaker_owner_requested=true` e `speaker_owner_block_reason=NO_TX`, esperado
  enquanto ainda nao havia frame de playback.
- Janela sem SAY real confirmou o dry-run em TX/silencio:
  `speaker_owner_handoff_ready=true`, `speaker_owner_block_reason=NONE`,
  zero failures/recoveries e `normal_path_clean=true`.
- Resposta real curta com dry-run armado validou o caminho SAY: `370/370`
  chunks SAY recebidos/tocados, zero drops, zero falhas de write/commit,
  `speaker_owner_handoff_ready=true`, `speaker_owner_active=true`,
  `speaker_owner_block_reason=NONE`, Audio IO sem drops/recoveries e Codec v2
  sem drops/erro. O pacote egress residual (`opus_egress_queue_count=1`) foi
  drenado apos o teste.
- Rollback operacional confirmado: `debug playback-v2 speaker-owner-disarm`
  voltou para `speaker_owner_dry_run_enabled=false`,
  `speaker_owner_requested=false`, `speaker_owner_block_reason=DISABLED`;
  `codec-v2 health` final voltou `healthy=true/status=ok`, fila egress zero.

Incremento N6.2 iniciado:

- O server ganhou o gate operacional
  `debug playback-v2 speaker-owner-gate`, que encapsula a sequencia
  `speaker-owner-arm -> playback-v2 delta -> readiness check -> speaker-owner-disarm`.
- O gate avalia o readiness exposto na N6.1 pelo namespace Playback v2:
  dry-run armado, candidato a speaker owner, `speaker_owner_handoff_ready`,
  `speaker_owner_block_reason=NONE`, zero failures/recoveries, delta SAY limpo,
  Audio IO sem drops/recoveries e Codec v2 saudavel.
- O desarme roda em `finally`, para preservar rollback operacional mesmo se a
  janela de delta ou a leitura de health falhar.
- O comando nao altera firmware, nao move HAL para Playback v2, nao toca wake,
  VAD, Capture, Activity, Codec, Opus ou PCM16. Ele apenas torna repetivel o
  ensaio que antes era feito manualmente por arm/delta/disarm.
- Uso esperado em hardware antes de qualquer owner real:
  `noisebot_server --host 192.168.1.30 debug playback-v2 speaker-owner-gate --wait-s 120 --no-prompt --json`;
  durante a janela, executar um turno real curto por wake e confirmar
  `ok=true`, `ready=true`, `block_reason=NONE`, `disarmed.ok=true` e deltas SAY
  recebidos/tocados sem drops.

Validacao N6.2 em hardware:

- Gate real de 120 s com turno curto por wake retornou `ok=true` e
  `status=warn` apenas por warnings operacionais conhecidos
  (`opus_egress_queue_count=1` e `heap_internal_free_kb=11`).
- Readiness de speaker owner ficou verde durante a janela:
  `ready=true`, `active=true`, `block_reason=NONE`,
  `speaker_owner_failures=0` e `speaker_owner_recoveries=0`.
- Playback v2 recebeu/tocou `186/186` chunks SAY no intervalo, com
  `say_chunks_dropped=0`, `speaker_write_failures=0`,
  `speaker_commit_failures=0`, `queue_empty=true` e
  `normal_path_clean=true`.
- Audio IO v2 nao registrou `dropped_frames`, `i2s_recoveries`, falhas ou
  recoveries de speaker handoff; Codec v2 tambem ficou sem drops/erro.
- Rollback operacional passou: `speaker-owner-gate` desarmou o owner no final
  (`disarmed.ok=true`, `speaker_owner_dry_run_enabled=false`,
  `speaker_owner_requested=false`, `speaker_owner_block_reason=DISABLED`).
- O pacote egress residual foi drenado depois (`drained_packets=1`) e
  `codec-v2 health` final voltou `healthy=true/status=ok`, sem issues/warnings,
  fila egress zero e `opus_codec_error=0`.

Incremento N6.3 iniciado:

- O gate `debug playback-v2 speaker-owner-gate` ganhou modo estrito de SAY real
  por `--require-say` e `--min-say-chunks N`.
- Sem esse modo, uma janela sem SAY continua servindo como ensaio de idle/TX e
  retorna apenas warning; com `--require-say`, o mesmo caso vira falha
  operacional, mesmo que readiness, Audio IO e Codec estejam verdes.
- Isso nao altera firmware, HAL, wake, VAD, Capture, Activity, Codec, Opus ou
  PCM16. O objetivo e impedir promocao baseada em um dry-run que so viu
  silencio/TX, deixando claro quando houve resposta real do bridge.
- Gate esperado em hardware:
  `noisebot_server --host 192.168.1.30 debug playback-v2 speaker-owner-gate --wait-s 120 --no-prompt --require-say --json`;
  durante a janela, executar um turno real curto por wake e aceitar somente
  `ok=true`, `required_say_chunks>=1`, deltas SAY recebidos/tocados, zero drops
  e `disarmed.ok=true`.

Observacao N6.3:

- A primeira repeticao com `--require-say` falhou corretamente quando nenhum
  SAY passou no intervalo (`0 < 1`), preservando rollback e health final limpo.
- A repeticao seguinte revelou um caso diferente: os contadores do firmware
  resetaram durante a janela (`say_chunks_received` caiu de 1889 para 0,
  `rx_owner_frames` caiu de 168634 para 2914 e o gate voltou `DISABLED` antes
  do snapshot final). O gate do server passou a detectar delta negativo como
  `counter_reset_detected=true` e reportar "possivel reboot/reset diagnostico",
  em vez de classificar isso como drop SAY comum ou falta de SAY real.
- Repeticao final do gate estrito com resposta real curta passou:
  `ok=true`, `required_say_chunks=1`, `counter_reset_detected=false`,
  `ready=true`, `active=true`, `block_reason=NONE`, `361/361` chunks SAY
  recebidos/tocados, zero drops, zero falhas de write/commit, Audio IO sem
  drops/recoveries e Codec v2 `healthy=true/status=ok`.
- O rollback final tambem passou: Playback v2 voltou para
  `speaker_owner_dry_run_enabled=false`, `speaker_owner_requested=false`,
  `speaker_owner_block_reason=DISABLED`, fila SAY zero e `ESP_OK`.

Incremento N6.4 iniciado:

- O gate estrito de Playback v2 passou a exigir, quando `--require-say` esta
  ativo, que o speaker owner fique `active=true` e observe pelo menos um frame
  nao silencioso (`speaker_owner_frames - speaker_owner_silence_frames > 0`).
- Isso evita aceitar um caso em que os contadores SAY avancam, mas o dry-run do
  owner do speaker so observou silencio/TX. O resultado passa a expor
  `non_silence_frames` para auditoria.
- A mudanca e server-only e nao altera firmware, HAL, wake, VAD, Capture,
  Activity, Codec, Opus ou PCM16; apenas fortalece o criterio de promocao antes
  de qualquer owner real.
- Observacao de bancada: uma repeticao N6.4 teve timeout no snapshot final de
  `/api/audio/playback-v2`; o pos-check confirmou estado seguro
  (`speaker_owner_*` desarmado, `380/380` SAY recebidos/tocados, zero drops,
  Audio IO sem recoveries e `codec-v2 health` ok). O gate do server passou a
  retornar `delta_error` estruturado e ainda tentar `speaker-owner-disarm`, em
  vez de soltar traceback quando o HTTP do firmware fica indisponivel no fim da
  janela.
- Repeticao N6.4 final passou no gate estrito: `ok=true`,
  `required_say_chunks=1`, `counter_reset_detected=false`, `delta_error=""`,
  `ready=true`, `active=true`, `block_reason=NONE`, `381/381` chunks SAY
  recebidos/tocados, `non_silence_frames=381`, zero drops, zero falhas de
  write/commit, Audio IO sem drops/recoveries e Codec v2 `healthy=true`.
- O pos-check confirmou rollback seguro: Playback v2 desarmado
  (`speaker_owner_dry_run_enabled=false`, `speaker_owner_requested=false`,
  `speaker_owner_block_reason=DISABLED`), fila SAY zero e Codec health
  `status=ok`, sem warnings.

Incremento N6.5 iniciado:

- O `speaker-owner-gate` passou a emitir um resumo de promocao:
  `real_owner_candidate`, `real_owner_candidate_status` e
  `real_owner_candidate_blockers`.
- O candidato so fica pronto quando o gate roda com `--require-say`, nao tem
  issues, nao tem `delta_error`, nao detecta reset de contadores, fica
  `speaker_owner_handoff_ready=true`, `speaker_owner_active=true`, observa
  `non_silence_frames>0` e confirma `speaker-owner-disarm` no rollback.
- Warnings operacionais, como heap interno baixo, deixam o candidato em
  `ready_with_warnings`; issues ou falta de SAY/active/non-silence bloqueiam.
- Esta etapa nao liga owner real e nao altera firmware/HAL. Ela apenas separa
  "dry-run saudavel" de "candidato forte o bastante para desenhar o proximo
  incremento controlado".
- Validacao N6.5 em hardware: o gate com `--require-say` observou resposta real
  (`374/374` chunks SAY recebidos/tocados, `non_silence_frames=374`,
  `ready=true`, `active=true`, `block_reason=NONE`, sem reset e sem falhas de
  Audio IO), mas bloqueou corretamente `real_owner_candidate=false` porque
  surgiram `say_chunks_dropped=3` no intervalo. O pos-check confirmou Playback
  v2 desarmado e fila zero; `codec-v2 health` ficou `warn` por 1 pacote egress
  residual, drenado em seguida, e voltou `healthy=true/status=ok`.

Incremento N6.6 iniciado:

- O bloqueio de promocao do N6.5 (`say_chunks_dropped=3`) foi tratado primeiro
  no server, sem alterar firmware, HAL, wake, captura, Activity v2 ou Codec v2.
- A documentacao operacional ja indicava pacing conservador de 18 ms, mas o
  default real do `OutputScheduler` ainda caia para 16 ms quando
  `NOISEBOT_TTS_SEND_INTERVAL_MS` nao estava definido. O default runtime foi
  alinhado para 18 ms, mantendo a variavel de ambiente como rollback/ajuste de
  bancada.
- `/ai/status` passou a expor `tts_output_scheduler` com fila alvo, intervalo
  de envio SAY, duracao fisica do chunk e rampa inicial. Isso permite confirmar
  apos restart que o processo em execucao carregou o pacing esperado antes de
  repetir o gate fisico.
- Objetivo do gate seguinte: repetir `speaker-owner-gate --require-say` com
  resposta real e exigir `real_owner_candidate=true` ou, no minimo, zero drops
  novos antes de desenhar qualquer incremento de owner real.
- Validacao N6.6 com server reiniciado no default de 18 ms: `/ai/status`
  confirmou `tts_output_scheduler.say_send_interval_ms=18.0`, o egress Opus foi
  drenado para zero e o `speaker-owner-gate --require-say` com transcript
  sintetico pelo Ops HTTP passou com `298/298` chunks SAY recebidos/tocados,
  zero drops, zero falhas de write/commit, `non_silence_frames=802`,
  `real_owner_candidate=true` e `real_owner_candidate_status=ready_with_warnings`
  apenas por `heap_internal_free_kb baixo: 11`. O rollback desarmou Playback v2
  e deixou a fila SAY final em zero.

Incremento N6.7 iniciado:

- Playback v2 ganhou um segundo nivel explicito de armamento:
  `speaker-owner/real-arm` e `speaker-owner/real-disarm`.
- O `real-arm` nao troca o dono fisico do HAL e nao faz Playback v2 chamar HAL
  diretamente. Ele apenas marca `speaker_owner_real_requested/armed` quando o
  preflight do owner ja esta forte: dry-run ligado, owner solicitado,
  `speaker_owner_handoff_ready=true`, `speaker_owner_active=true`, frames SAY
  nao silenciosos observados, zero falhas e zero recoveries.
- Se o preflight nao estiver verde, `real-arm` retorna conflito e publica
  `speaker_owner_real_block_reason` (`DISABLED`, `NO_TX`, `TX_ERROR` ou
  `I2S_RECOVERY`). Esse passo cria o corrimao para o owner real sem alterar
  wake, captura, codec, Activity v2, fila SAY ou escrita fisica do speaker.
- Validacao inicial apos flash confirmou baseline seguro
  (`speaker_owner_real_armed=false`, fila SAY zero) e bloqueio correto de
  `real-arm` sem gate (`DISABLED`). Uma fala sintetica com dry-run armado
  mostrou preflight ativo, mas tambem revelou `say_chunks_dropped=12`; o
  `real-arm` foi reforcado para bloquear qualquer drop SAY/listening ou falha
  de write/commit como `TX_ERROR`.
- Validacao apos o flash do reforco fechou o aceite do corrimao: baseline com
  `speaker_owner_real_armed=false`, `real-arm` sem gate bloqueado como
  `DISABLED` e sem traceback no CLI, Codec v2 `status=ok`, dry-run armado em
  fala real via Ops API com 360 chunks SAY recebidos/tocados, zero drops,
  zero falhas de write/commit, `speaker_owner_handoff_ready=true`; so entao
  `speaker-owner-real-arm` armou (`speaker_owner_real_armed=true`). O teste
  terminou com `real-disarm` e `speaker-owner-disarm`, deixando
  `speaker_owner_real_armed=false`, dry-run desligado e fila SAY final zero.

Incremento N6.8 iniciado:

- Playback v2 ganhou telemetria de execucao armada real:
  `speaker_owner_real_write_frames`, `speaker_owner_real_write_samples`,
  `speaker_owner_real_write_failures` e `speaker_owner_real_last_result`.
- Esses contadores so sobem enquanto `speaker_owner_real_armed=true` e o frame
  SAY e escrito pelo callback fornecido por `audio_service`. O HAL fisico ainda
  permanece no `audio_service`; Playback v2 nao chama `audio_hal_*` diretamente.
- O objetivo e validar um turno completo com `real-arm` ja armado antes da fala
  e confirmar que frames/samples reais aparecem nesses contadores com zero
  falhas, fila SAY final zero, Codec v2 saudavel e rollback por `real-disarm`
  + `speaker-owner-disarm`.
- Validacao apos flash fechou o incremento: baseline pos-flash trouxe
  `speaker_owner_real_write_*` zerados; a primeira fala com dry-run armado
  enviou 372 chunks SAY e manteve `speaker_owner_real_write_frames=0` porque
  `real-arm` ainda nao estava armado. Depois de `speaker-owner-real-arm`, a
  segunda fala enviou 371 chunks SAY e os contadores reais subiram para
  `speaker_owner_real_write_frames=371`,
  `speaker_owner_real_write_samples=94976`,
  `speaker_owner_real_write_failures=0` e ultimo resultado `ESP_OK`. A fila
  SAY ficou zero, Codec v2 ficou `status=ok`, e o rollback final por
  `real-disarm` + `speaker-owner-disarm` voltou para estado seguro.

Incremento N6.9 iniciado:

- O `speaker-owner-real-arm` agora abre uma janela real controlada de playback.
  Durante essa janela, os writes continuam passando pelo callback de
  `audio_service`; Playback v2 ainda nao chama HAL diretamente.
- Quando o fim de SAY e confirmado por `audio_playback_service_v2_say_end_idle()`,
  Playback v2 auto-desarma `speaker_owner_real_requested/armed`, marca
  `speaker_owner_real_window_completed=true` e incrementa
  `speaker_owner_real_auto_disarm_count`, preservando
  `speaker_owner_real_write_*` para leitura pos-turno.
- Aceite apos flash: armar dry-run, gerar uma fala limpa para deixar o gate
  verde, armar `real-arm`, gerar uma segunda fala e confirmar que, ao fim dela,
  `speaker_owner_real_armed=false`,
  `speaker_owner_real_window_completed=true`,
  `speaker_owner_real_auto_disarm_count=1`, `speaker_owner_real_write_frames`
  igual aos chunks SAY da segunda fala, zero falhas, fila SAY zero e Codec v2
  saudavel.
- Primeira tentativa apos flash confirmou os campos novos e o bloqueio seguro,
  mas nao fechou o aceite: uma rodada com baseline sujo acumulou drops antigos
  e bloqueou `real-arm` como `TX_ERROR`; apos reboot limpo, a fala de gate
  ainda gerou drops SAY (`say_chunks_dropped=35`). Com pacing em 20 ms, o
  dry-run ficou limpo (`388/388`, zero drops), mas a janela real auto-desarmou
  com 344 frames reais e ainda acumulou 48 drops SAY. O proximo ajuste e
  firmware/diagnostico: nao subir mais o pacing, porque 24 ms piorou a
  continuidade perceptivel do audio; investigar janela/estado/aceite dos
  chunks SAY antes de repetir a janela N6.9.
- Correcao N6.9 seguinte: manter o server em 20 ms e adicionar backpressure
  curto no `audio_playback_service_v2_say_accept()`. O accept ainda tenta
  enfileirar imediatamente, mas quando a fila SAY esta cheia espera ate 16 ms
  (um chunk fisico) antes de dropar. `/api/audio/playback-v2` passa a expor
  `say_queue_high_watermark`, `say_accept_wait_ms`,
  `say_chunks_queue_full`, `say_chunks_queue_wait_recovered` e
  `say_chunks_dropped_queue_full` para separar jitter recuperado de drop real.
- Validacao final apos flash de `c045a57`: `/api/audio/playback-v2` voltou a
  responder com os campos novos e baseline limpo. Com server em 20 ms, o
  dry-run recebeu/tocou 393/393 chunks SAY, zero drops, fila final zero,
  `say_queue_high_watermark=32` e 237 eventos de fila cheia recuperados por
  wait. O `speaker-owner-real-arm` abriu a janela real; a segunda fala
  auto-desarmou com `speaker_owner_real_window_completed=true`,
  `speaker_owner_real_auto_disarm_count=1`,
  `speaker_owner_real_write_frames=393`,
  `speaker_owner_real_write_samples=100608`, zero write/commit failures,
  `say_chunks_received=786`, `say_chunks_played=786`,
  `say_chunks_dropped=0`, `say_chunks_dropped_queue_full=0`, fila SAY zero e
  `codec-v2 health` saudavel (`healthy=true`, drops zero). O disarm final
  deixou o owner dry-run e real desarmados. N6.9 fechado.

Fechamento N6:

- N6 esta fechada como ownership controlado de speaker/HAL por gate e janela
  real, ainda sem chamada direta de `audio_hal_*` dentro de Playback v2.
- O caminho fisico continua no callback fornecido por `audio_service`, mas
  Playback v2 agora domina a fila SAY, preparo/commit/write por contrato,
  readiness, real-arm, janela real, auto-disarm, contadores reais e
  backpressure de accept.
- Avanco operacional pos-N6: o CLI `debug playback-v2
  speaker-owner-real-window-gate` automatiza a sequencia
  `dry-run gate -> real-arm -> real-window delta -> disarm`, sempre tentando
  rollback no final. Isso torna a validacao N6.9 repetivel sem novo firmware e
  deve ser usado como regressao antes de qualquer nova reducao do
  `audio_service.c`.
- Correcao server-only pos-N6 para voz picotada: manter apenas
  `NOISEBOT_TTS_SEND_INTERVAL_MS=20` reduzia overflow, mas subalimentava o
  speaker porque o chunk fisico toca 16 ms. O default do `OutputScheduler`
  passou para prebuffer curto de 4 chunks (`NOISEBOT_TTS_QUEUE_TARGET=4`) e
  cadencia nominal de 16 ms (`NOISEBOT_TTS_SEND_INTERVAL_MS=16`), preservando
  headroom da fila SAY v2 de 32 chunks sem alongar artificialmente o audio.
- Validacao final pos-picote em hardware, apos restart do server: frase curta
  (`ww -> me fale uma frase curta`) e frase media (`ww -> me conte uma
  historia`) passaram com Opus ativo, `firmware_say_queue_target=4`,
  `say_send_interval_ms=16.0`, `say_chunks_received=1908`,
  `say_chunks_played=1908`, `say_queue_count=0`, `say_chunks_dropped=0`,
  `speaker_write_failures=0`, `speaker_commit_failures=0` e
  `codec-v2 health` saudavel. Incremento pos-N6 fechado.

## Estado Consolidado Pos-N6

Leitura operacional em 2026-06-03:

- N1 Capture v2 esta fechada como TX owner controlado: Capture v2 pode assumir
  o TX real da sessao por flag/config, com rollback para o caminho legado do
  `audio_service` e zero drops nas validacoes de turno curto, barge/pare,
  no-echo, PCM16 rollback e Opus v2.
- N2 Activity v2 esta verde como comparador dentro de sessoes reais, mas ainda
  nao e o decisor principal de fim de fala. Esta e a principal pendencia
  funcional antes de declarar o Voice Audio v2 inteiro como fechado.
- N3 Audio IO v2 esta fechada para RX distribuido, TX observado, telemetria de
  recovery e gate/dry-run de speaker. O HAL fisico ainda permanece protegido
  pelo `audio_service`.
- N4 Playback v2 esta fechada para contrato de speaker/SAY: Playback v2 fornece,
  prepara, commita e orquestra o write por callback, mas nao chama
  `audio_hal_*` diretamente.
- N5 esta fechada como reducao segura do `audio_service.c` no caminho
  SAY/Playback v2. Ainda restam estado legado, eventos, wake rearm e callback
  fisico do HAL de forma intencional.
- N6 esta fechada como ownership controlado por gate/janela real, com
  auto-disarm, backpressure SAY, regressao CLI e baseline falado pos-picote.

Proxima pendencia recomendada: promover Activity v2 de comparador para decisor
controlado apenas dentro de sessao ja aberta por wake/barge, mantendo rollback
imediato para o VAD legado do `audio_service`. Antes de qualquer novo toque em
HAL/I2S, tratar `heap_internal_free_kb`/`heap_dma_free_kb` baixos como risco de
estabilidade a ser monitorado nos gates.

Incremento N2A iniciado em 2026-06-03: foi adicionada a flag persistente
`voice_audio_v2_activity_decider_enabled`, default `false`, exposta em
`/api/config`, `/api/config/all` e como `activity_decider_enabled` em
`/api/audio/activity-v2`. Esta etapa ainda nao altera o dono real da decisao:
Activity v2 continua comparador/passivo e o VAD legado do `audio_service`
permanece responsavel por encerrar a sessao ate a promocao controlada N2B.

Incremento N2B local em 2026-06-03: com a flag ligada, o `audio_service`
consulta `voice_activity_service_v2_session_end_observed()` depois do frame de
Activity v2 e antes do VAD legado. A promocao fica limitada a sessoes ja
abertas e com voz detectada; nao abre sessao em IDLE, nao altera wake, captura,
codec, playback ou HAL. `/api/audio/activity-v2` passa a expor
`activity_decider_owner_active`, `activity_decider_end_used`,
`activity_decider_end_count` e `activity_decider_end_elapsed_ms` para validar o
owner real e manter rollback por `voice_audio_v2_activity_decider_enabled=false`.

Validacao hardware N2B em 2026-06-03: apos flash, baseline com a flag desligada
confirmou `activity_decider_enabled=false`, `activity_decider_end_count=0`,
Playback v2 com fila SAY zero e `codec-v2` sem drops. Com
`voice_audio_v2_activity_decider_enabled=true`, o turno curto
`ww -> que horas sao` terminou com `activity_decider_end_used=true`,
`activity_decider_end_count=1`, `activity_end_observed=true`,
`decision_diverged=false`, Capture v2 TX owner ativo sem drops,
Playback v2 `say_chunks_received=370`, `say_chunks_played=370`,
`say_chunks_dropped=0`, Codec v2 `packet_drops=0`/egress zero, e server
`last_outcome=local_intent` com transcript `Que horas são?`. A flag foi
desligada no final para rollback operacional.

Gate medio N2B em 2026-06-03: com a flag ligada, `ww -> me conte uma historia
curta` manteve server/Playback/Codec saudaveis, mas expôs divergencia do
decisor: o VAD legado encerrou antes, enquanto Activity v2 ainda nao tinha
atingido os 900 ms consecutivos de silencio (`silence_run_frames=38`, ~608 ms).
A janela de fim de fala do Activity v2 foi reduzida para 600 ms e
`activity_end_silence_ms` foi adicionado ao status para validar o novo gate sem
inferir pela contagem de frames.

Reteste hardware do gate medio apos flash: com
`voice_audio_v2_activity_decider_enabled=true`, `ww -> me conte uma historia
curta` passou com `activity_decider_end_used=true`,
`activity_decider_end_count=1`, `activity_end_silence_ms=608`,
`decision_diverged=false`, Capture v2 TX owner ativo sem drops, Playback v2
`say_chunks_received=1017`, `say_chunks_played=1016`, `say_chunks_dropped=0`
e Codec v2 `packet_drops=0`. O server respondeu por LLM com transcript
`Me conte uma história curta.`. A flag foi desligada ao final para rollback.

Gate `pare` N2B em 2026-06-03: com a flag ligada, `ww -> pare` passou sem
resposta aleatoria e sem LLM. O Activity v2 encerrou o turno
(`activity_decider_end_used=true`, `activity_end_silence_ms=608`,
`decision_diverged=false`), Capture v2 ficou `SPEECH_COMPLETE` porque o
cancelamento e uma politica local do server apos STT, nao um `CANCELLED`
firmware, e o server classificou `intent_name=local_stop`,
`turn_taking_decision=post_barge_stop`, `last_reply=Pronto, parei.`. Playback v2
ficou sem overflow/drop de fila; os `say_chunks_dropped_listening=109` foram
descartes de SAY durante escuta/barge-stop, esperados nesse caminho. Codec v2
permaneceu sem `packet_drops`/egress drops. A flag foi desligada ao final.

Gate barge/no-echo N2B em 2026-06-03: com a flag ligada, o roteiro
`ww -> me conte uma historia longa`, interrupcao durante fala com `ww -> pare`
e silencio posterior passou. `/ai/metrics` registrou o turno da historia
(`turn_id=11`) como `outcome=interrupted` e `discard_reason=barge_in`, seguido
do turno `pare` (`turn_id=12`) como `local_stop`, `direct_stop`,
`last_reply=Pronto, parei.` e sem LLM. Nenhum novo turno apareceu apos a janela
manual de silencio. No firmware, Activity v2 encerrou sem divergencia
(`activity_decider_end_used=true`, `activity_end_silence_ms=608`,
`decision_diverged=false`), Capture v2 marcou `source=BARGE_IN` com TX owner e
zero drops, Playback v2 ficou sem overflow/drop de fila (`say_queue_count=0`,
`say_chunks_dropped_queue_full=0`; drops de listening/cancel esperados no
barge-stop), e Codec v2 terminou com `packet_drops=0`,
`opus_egress_packet_drops=0` e fila egress zero. A flag foi desligada ao final.
Com curto, medio, `pare` e barge/no-echo verdes, N2 esta fechada como decisor
controlado opt-in.

Incremento N2C local em 2026-06-03: Activity v2 foi promovido de opt-in manual
para default controlado. `NB_CFG_DEFAULT_V2_ACT_DEC` passa a `1` e o
`config_manager` aplica uma migracao one-shot via `v2act_mig`, para que placas
que ja tinham `v2act_dec=false` por rollback manual sejam promovidas no primeiro
boot deste firmware. Depois da migracao, `voice_audio_v2_activity_decider_enabled=false`
continua sendo rollback persistente e nao e religado automaticamente.

Validacao hardware N2C em 2026-06-03: apos flash do commit `5b43f6c`,
`/api/audio/activity-v2` e `/api/config/all` retornaram
`activity_decider_enabled=true` sem arme manual, confirmando a migracao
one-shot. Capture v2 permaneceu idle com TX owner configurado, Playback v2 com
fila SAY zero e zero drops, e Codec v2 com worker ativo e zero drops. O rollback
por `/api/config` foi testado: desligar retornou `activity_decider_enabled=false`
e religar retornou `true`, sem sessao ativa e com `ESP_OK`.

Incremento local seguinte: foi criado o gate consolidado
`GET /api/audio/voice-v2`, sem mudar runtime de audio. Ele agrega status de
Capture v2, Activity v2, Playback v2, Codec v2 e Audio IO v2 em um unico JSON
com `ready`, `block_reason` e `rollback_available`. O gate considera prontos os
defaults controlados (`capture`, `capture_tx`, `activity_decider`), o worker do
Codec v2 ativo, Playback v2 como dono da fila SAY, runtime idle, filas vazias,
zero drops/recoveries e ausencia de falhas de speaker. A intencao e congelar um
snapshot de preflight antes de nova reducao estrutural do `audio_service.c`, sem
trocar HAL, wake, VAD, captura ou protocolo.

Validacao hardware do gate consolidado em 2026-06-03: apos flash do commit
`7a45a6b`, `GET /api/audio/voice-v2` retornou `ready=true` e
`block_reason=none`. O snapshot confirmou `capture_enabled=true`,
`capture_tx_enabled=true`, `activity_decider_enabled=true`, Audio IO/Activity/
Playback inicializados, Codec v2 worker `running`, Playback v2 como dono da
fila SAY, runtime idle, `playback_say_queue_count=0`, zero drops/falhas de
Playback, `codec_queue_count=0`, `codec_egress_queue_count=0`, zero drops de
Codec e zero recoveries no Audio IO. O server local estava conectado com Opus
ativo; `codec-v2 health` retornou `healthy=true`, `status=ok`, sem
issues/warnings, `opus_codec_error=0` e egress zero. Capture v2 estava idle com
captura/TX armados e Activity v2 default ligado, sem sessao ativa.

Incremento server-only seguinte: o gate consolidado ganhou proxy operacional em
`GET /api/device/audio/voice-v2` e CLI `debug voice-v2 status`. O comando usa o
mesmo endpoint do firmware, sem alterar runtime, HAL, wake, VAD, captura,
playback, codec, Opus ou PCM16. Validacao local passou em
`server/tests/test_server_facade.py` (`199 passed`) e o CLI contra hardware
retornou `ready=true`, `block_reason=none`, filas/drops zerados e Codec v2
worker `running`. O proxy HTTP do server requer restart do processo para a nova
rota ficar visivel na instancia ja aberta.

Incremento server-only seguinte: `voice-release-check` passou a incluir o gate
`Voice v2 consolidado` como primeiro criterio. O release check agora carrega
`voice_v2` no JSON e falha quando `ready=false` ou `block_reason` difere de
`none`, antes dos gates especificos de Codec, Capture, Playback e metricas.
Validacao local passou em `server/tests/test_server_facade.py` (`200 passed`) e
validacao real contra hardware/server local retornou `ok=true` com
`Voice v2 consolidado: ready=True, block=none`, Codec v2 healthy, Capture v2
controlado idle, Playback v2 SAY com fila zero e metricas sem falha.

Incremento firmware seguinte: Playback v2 ganhou lifecycle explicito do SAY
real. `audio_service` chama `audio_playback_service_v2_say_begin()` quando a
fala do bridge entra em `PLAY_BRIDGE_SAY`; Playback v2 marca
`bridge_say_active=true`, incrementa `say_begin_count` e fecha o ciclo com
`say_end_count` no idle normal, cancelamento ou descarte por nova escuta. Os
campos aparecem em `/api/audio/playback-v2`, e o gate consolidado
`/api/audio/voice-v2` tambem expoe `playback_say_active`,
`playback_say_begin_count` e `playback_say_end_count`. Enquanto
`bridge_say_active=true`, o preflight trata o runtime como ocupado
(`playback_active`) mesmo se a fila estiver momentaneamente vazia. Isso reduz
falso idle entre jitter/fim de fila sem mudar wake, VAD, captura, codec,
bridge, Opus/PCM16 ou ownership fisico do HAL, que continua no `audio_service`.

Validacao hardware desse lifecycle: apos flash, baseline voltou
`voice-release-check ok=true`, `/api/audio/voice-v2 ready=true` e
`block_reason=none`. No turno real `ww -> uma frase curta`, Playback v2
registrou `say_begin_count=1`, `say_end_count=1`, 345 chunks SAY
recebidos/tocados, fila final zero, zero drops, zero falhas de write/commit e
`speaker_idle_end_count=1`. O server confirmou `tts_completed=true`,
`tts_say_end_sent=true`, 3/3 paginas visuais enviadas e Opus v2 ativo. O
release-check pos-turno permaneceu `ok=true`; o unico aviso foi o estado DONE
retido do Capture v2 controlado, com sessao inativa e zero drops.

Incremento server-only seguinte: `voice-release-check` passou a validar o
lifecycle SAY do Playback v2. O gate `Playback v2 SAY` agora considera falha
se `bridge_say_active=true` no preflight ou se `say_begin_count` e
`say_end_count` estiverem desencontrados. Isso transforma a validacao do turno
real em regressao automatica antes de novos cortes em `audio_service.c`, sem
alterar firmware, HAL, wake, VAD, captura, codec, Opus ou PCM16.

Incremento firmware seguinte: o `audio_service.c` passou a concentrar as
transicoes legadas de inicio/fim de SAY nos helpers internos
`audio_service_begin_bridge_say_playback()` e
`audio_service_finish_bridge_say_playback()`. O fluxo continua identico: o
inicio de `PLAY_BRIDGE_SAY` so acontece depois de
`audio_playback_service_v2_say_accept()` aceitar o primeiro chunk; Playback v2
continua dono do lifecycle/fila/contadores; `audio_service` ainda mantem
`NB_AUDIO_EVT_PLAYBACK_START/END`, `wake_service_rearm()`, a transicao
`PLAY_BRIDGE_SAY -> PLAY_IDLE` e o callback fisico para HAL. A mudanca reduz a
superficie do loop legado sem alterar wake, VAD, captura, codec, Opus/PCM16,
bridge ou ownership fisico do speaker.

Validacao pos-flash do incremento: turno real curto (`ww -> fale uma frase
curta`) manteve o contrato. Playback v2 fechou `say_begin_count=1` e
`say_end_count=1`, recebeu/tocou `216/216` chunks SAY, fila final zero, zero
drops, zero falhas de write/commit e `last_error=ESP_OK`. O
`voice-release-check` retornou `ok=true`: Voice v2 pronto, Playback v2 SAY
`active=false`, `begin/end=1/1`, metricas do turno `17` com
`tts_completed=true`, `tts_say_end_sent=true` e texto visual em `2/2` paginas.
Uma repeticao operacional a aproximadamente 4 m tambem passou: turno `18`
transcrito como `Que horas são?`, `local_intent`, TTS completo e Playback v2
acumulado em `say_begin_count=2`/`say_end_count=2`, `585/585` chunks
recebidos/tocados, fila zero, zero drops e `voice-release-check ok=true`.

Incremento firmware seguinte: o tratamento legado de `PLAY_STOP` tambem saiu
do corpo principal do `audio_task` e foi concentrado em
`audio_service_handle_play_stop()`. O helper preserva exatamente o fluxo atual:
fecha WAV aberto, cancela SAY ativo em Playback v2, escreve silencio curto,
emite `NB_AUDIO_EVT_PLAYBACK_END` e devolve `play_state` para `PLAY_IDLE`.
Nao muda wake, VAD, captura, codec, Opus/PCM16, fila SAY ou ownership fisico
do speaker; apenas reduz mais um bloco de transicao legada no loop.

Validacao pos-flash do incremento `PLAY_STOP`: baseline `voice-release-check`
ficou `ok=true` apos reboot, com Voice v2 pronto, Capture v2 em
`IDLE_SESSION`, Playback v2 zerado e sem drops. Turno real curto posterior
(`ww -> uma frase curta`) transcreveu `Uma frase curta.`, completou LLM/TTS,
enviou `SAY_END`, manteve texto visual em `2/2` paginas e fechou Playback v2
com `say_begin_count=1`, `say_end_count=1`, `259/259` chunks SAY
recebidos/tocados, fila zero, zero drops e zero falhas de write/commit. O
`voice-release-check` pos-turno permaneceu `ok=true`; unico aviso foi o DONE
retido conhecido do Capture v2.

Incremento firmware seguinte: o playback local `PLAY_ACTIVE` (WAV/PCM raw)
tambem saiu do corpo principal do `audio_task` e foi concentrado em
`audio_service_play_active_chunk()`. O helper preserva abertura de arquivo,
parse de WAV, evento `NB_AUDIO_EVT_PLAYBACK_START`, aplicacao de volume,
padding do ultimo chunk, write fisico via `audio_hal_spk_write()` e fechamento
por EOF com `NB_AUDIO_EVT_PLAYBACK_END`. Nao altera Voice v2, wake, VAD,
captura, codec, fila SAY ou ownership do speaker; apenas tira o caminho local
de asset do loop que tambem hospeda o pipeline v2.

Validacao pos-flash do incremento `PLAY_ACTIVE`: baseline ficou
`voice-release-check ok=true`. O turno real seguinte foi feito de longe, com TV
e ruido de sala; o STT transcreveu de forma distorcida (`Uma frara que curta.`)
mas marcou qualidade boa e o pipeline completou LLM/TTS/SAY_END. Playback v2
fechou `say_begin_count=1`, `say_end_count=1`, `273/273` chunks SAY
recebidos/tocados, fila zero, zero drops e zero falhas. O primeiro health
pos-turno viu 1 pacote Opus egress residual; `codec-v2 egress-drain` drenou 1
pacote e o `voice-release-check` final voltou `ok=true`, com Codec v2
`healthy=true/status=ok`.

Incremento firmware seguinte: o fallback TX quando nenhum caminho ativo escreveu
audio saiu do corpo principal do `audio_task` e foi concentrado em
`audio_service_fill_idle_output()`. O helper preserva a ordem atual:
`audio_playback_service_v2_fill_probe_chunk()`, `synth_fill_chunk()` e, por fim,
`audio_hal_spk_write_silence()`, sempre registrando o resultado em
`audio_note_spk_result()`. Nao muda Voice v2, wake, VAD, captura, codec, fila
SAY ou ownership fisico do speaker; apenas reduz o acoplamento do loop TX/RX.

Validacao pos-flash do fallback TX isolado: baseline inicial ficou verde e o
turno real 23 (`Me fale uma frase curta.`) completou em Opus v2 com
`tts_completed=true`, `SAY_END`, transcript_quality `good` e resposta curta.
Playback v2 fechou `say_begin_count=2`, `say_end_count=2`, `653/653` chunks SAY
recebidos/tocados, fila final zero, zero drops, zero falhas de write/commit e
high-watermark 14/32. O primeiro gate viu 1 pacote Opus egress residual; apos
`codec-v2 egress-drain`, o `voice-release-check` final voltou `ok=true` com
Codec v2 `status=ok`, `opus_egress_queue_count=0`, Capture v2 sem drops e
Audio IO v2 sem recoveries.

Fechamento firmware de reducao estrutural: o caminho RX restante do
`audio_task` foi concentrado em `audio_service_process_rx_chunk()`. O helper
mantem a ordem original de leitura do mic, condicionamento, conversao para
WakeNet/sound analysis, dispatch RX v2, timeouts de sessao e gravacao
diagnostica. Nao muda wake, VAD, Capture v2, Activity v2, Codec v2, Playback
v2, bridge, HAL ou ownership fisico; o ganho e fechar o bloco pos-N6 deixando o
loop principal como orquestrador curto TX -> RX.

Validacao pos-flash da reducao RX: baseline pre-turno `voice-v2` e
`voice-release-check` ficaram verdes. O turno real 24 (`Me fale uma frase
curta.`) completou em Opus v2 com transcript_quality `good`,
`tts_completed=true`, `SAY_END`, 2/2 paginas de texto visual e resposta curta.
Playback v2 fechou `say_begin_count=1`, `say_end_count=1`, `213/213` chunks SAY
recebidos/tocados, fila final zero, high-watermark 6/32, zero drops e zero
falhas de write/commit. Capture v2 enviou 58 chunks / 55680 samples com zero
drops. O primeiro gate viu 1 pacote Opus egress residual; apos
`codec-v2 egress-drain`, o `voice-release-check` final voltou `ok=true` com
Codec v2 `status=ok`, fila egress zero e Audio IO v2 sem recoveries.

## Ordem Recomendada

1. Fase I: playback v2 como dono gradual do downlink.
2. Fase M parcial: checklist/health de release para proteger o que ja ficou
   bom.
3. Fase J: voice activity v2 em shadow/opt-in, sem AEC.
4. Fase K: capture session v2 assume upstream por flag.
5. Fase L: policy conversacional avancada, so depois de no-echo e captura
   estarem estaveis.
6. Estado atual: Fase N0-N6 consolidada, Activity v2 promovido para default
   controlado com rollback e reducao estrutural pos-N6 do `audio_task`
   fechada por helpers internos. O proximo incremento deve ser uma fase nova,
   com preflight em `/api/audio/voice-v2`, e nao mais uma extensao infinita de
   N6.

Essa ordem segue a regra central da arquitetura v2: separar I/O, playback,
processor, codec e policy. No NoiseBot, a prioridade imediata e diminuir o
acoplamento do `audio_service.c` sem trocar wake, VAD, AEC e follow-up no mesmo
movimento.

## Fase O - Fechamento Operacional Voice Audio v2

Objetivo: sair da sequencia de handoffs N e transformar o estado atual em
criterios operacionais de fechamento, sem tocar wake, VAD, HAL/I2S, bridge ou
codec no mesmo passo.

### O0 - Hardening de Gates

Incremento server-only inicial: `voice-release-check` passou a avisar quando
`/api/audio/voice-v2` reporta `audio_io_heap_internal_free_kb` ou
`audio_io_heap_dma_free_kb` entre 1 e 15 KB. O gate continua `ok=true` se todos
os demais criterios estiverem verdes, mas o risco de heap baixo fica explicito
no JSON/Markdown antes de qualquer novo toque em HAL/I2S. Valor `0` e tratado
como nao informado, porque alguns snapshots de boot ainda retornam zero nesses
campos. Validacao local: `server/tests/test_server_facade.py -k
voice_release_check` com 9 testes verdes.

Incremento firmware/server seguinte: Audio IO v2 passou a expor tambem
`heap_internal_free_bytes`, `heap_dma_free_bytes`,
`heap_internal_largest_free_block` e `heap_dma_largest_free_block` em
`/api/audio/io-v2`, e os mesmos dados com prefixo `audio_io_` em
`/api/audio/voice-v2`. O `voice-release-check` agora prefere bytes/maior bloco
quando esses campos existem e preserva fallback por KB para firmware antigo.
Isso nao altera wake, captura, playback, codec, bridge nem HAL; apenas separa
heap baixo real de arredondamento/fragmentacao antes de qualquer proxima troca
estrutural. Validacao local: contratos firmware focados com 15 testes verdes,
`server/tests/test_server_facade.py -k voice_release_check` com 9 testes
verdes, e `idf.py build` limpo.

Incremento O0.1 de headroom: o primeiro corte de memoria evita mexer no audio e
atua no perfil de rede. `sdkconfig.defaults` passa a habilitar
`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` e reduz
`CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM` de 16 para 8. A intencao e deslocar
buffers WiFi/LwIP elegiveis para PSRAM e reduzir DRAM interna reservada para
TX WiFi, preservando o bridge TCP local e o dashboard. Rollback: reverter o
commit de config se WiFi/bridge ficar instavel apos flash. Validacao local:
contratos firmware focados com 15 testes verdes e `idf.py build` limpo.

Validacao O0.1 em hardware apos flash: `/api/audio/voice-v2` passou de cerca de
5 KB livres de heap interno/DMA para cerca de 18,9 KB, com maior bloco livre em
17 KB. O `voice-release-check` final retornou `ok=true`, sem warnings de heap:
Voice v2 `ready=true`, `block_reason=none`, runtime idle, Codec v2 worker
`running`, Playback v2 com fila SAY zero, zero drops/falhas, Capture v2 inativo
e Audio IO v2 sem recoveries. Em turno real curto, o server registrou
`transcript_quality=good`, `outcome=llm`, `tts_completed=true`,
`tts_say_end_sent=true`, texto visual `3/3` paginas e Playback v2
`305/305` chunks SAY recebidos/tocados, sem drops. Um pacote Opus egress
residual apareceu no primeiro gate pos-turno, foi drenado por
`codec-v2 egress-drain`, e o gate final permaneceu verde. O0.1 fica fechado.

### O1 - Saneamento de Egress Residual

Objetivo: remover a necessidade operacional de drenar manualmente um pacote
Opus egress residual depois de alguns turnos reais, sem alterar captura,
playback, wake, VAD, bridge, HAL/I2S, Opus upstream ou rollback PCM16.

Diretriz: tratar o egress residual como saneamento de fila diagnostica/worker,
nao como falha de fala. O incremento deve preferir limpeza em ponto idle seguro
ou criterio explicito no gate, com telemetria de quantos pacotes foram limpos,
mantendo falha real para drops, erro de codec, worker parado ou egress crescente.

Aceite:

- `voice-release-check` permanece `ok=true` apos turno real sem passo manual de
  `egress-drain`.
- `codec-v2 health` continua falhando para drops ou `opus_codec_error`.
- Playback v2 continua `SAY_BEGIN/SAY_END` balanceado, fila SAY zero e zero
  drops/falhas.
- PCM16 rollback continua intacto.

Incremento O1 server-only implementado: `voice-release-check` agora aplica
auto-drain conservador de egress Opus quando encontra exatamente 1 pacote
residual, Codec v2 sem issues/drops/erro, Voice v2 `ready=true`, runtime idle,
Capture v2 sem sessao ativa e Playback v2 sem SAY ativo/fila. Depois do drain,
o check relê `/api/audio/voice-v2` e `codec-v2 health`, anota
`auto_egress_drain`, `auto_egress_drained_packets` e a fila antes/depois no
JSON, e mantém warning informativo no gate Codec. Fila maior que 1 pacote,
drops, `opus_codec_error`, worker ruim ou runtime ocupado continuam sem
auto-drain e permanecem como warning/falha normal. O endpoint HTTP operacional
do server usa a mesma regra. Validacao local: `server/tests/test_server_facade.py`
com 204 testes verdes. Validacao live sem residual pendente: release-check
continuou `ok=true`, heap interno/DMA ~18,8 KB e Codec/Playback/Capture verdes.

Fechamento final da Fase O em hardware: apos reiniciar o server com o codigo
O1 carregado, o health local confirmou restart real (`uptime_s` baixo). O teste
fisico `ww -> fale uma frase curta` completou como `turn_id=1`, transcript
`Fale uma frase curta.`, `transcript_quality=good`, `outcome=llm`,
`tts_completed=true`, `tts_say_end_sent=true`, texto visual `1/1` pagina e
`voice_alert=null`. Playback v2 fechou `say_begin_count=1`,
`say_end_count=1`, `158/158` chunks SAY recebidos/tocados, fila zero, zero
drops e zero falhas de write/commit. Capture v2 ficou `DONE`,
`SPEECH_COMPLETE`, 29 chunks / 27840 samples, zero drops. Codec v2 ficou
`status=ok`, worker `running`, egress zero, zero drops e
`opus_codec_error=0`; Audio IO v2 manteve cerca de 19 KB livres interno/DMA,
maior bloco 17 KB e zero recoveries. O `voice-release-check` final retornou
`ok=true` sem necessidade de dreno manual. Fase O fechada.

## Fase P - Audio Service Como Ponte/Compatibilidade

Objetivo: depois do fechamento operacional da Fase O, reduzir o
`audio_service.c` sem trocar novamente o caminho real de audio no mesmo passo.
A meta de P nao e "mais teste": e explicitar ownership residual e transformar o
`audio_service` em ponte/compatibilidade, mantendo HAL fisico e rollback v1
seguros ate cada owner v2 estar maduro.

### P0 - Baseline Pos-O

Baseline congelado:

- `voice-release-check ok=true` apos restart real do server.
- Opus v2 segue como default local do server, PCM16 segue rollback.
- Capture v2 e Activity v2 estao como owners controlados/defaults com rollback
  por config, mas ainda preservam compatibilidade v1.
- Playback v2 e dono de fila/lifecycle/preparo/commit/write orchestration por
  callback; o HAL fisico ainda nao foi movido para Playback v2.
- Audio IO v2 observa/distribui RX/TX e reporta recoveries/heap; HAL/I2S fisico
  ainda esta no `audio_service`.
- Heap interno/DMA pos-O ficou em torno de 19 KB livres, maior bloco 17 KB,
  sem warning de release.

### P1 - Mapa de Ownership Restante

Responsabilidades que ainda devem ficar no `audio_service.c` por enquanto:

- Escrita/leitura fisica no HAL/I2S (`audio_hal_*`) e recovery de I2S.
- Callback de eventos legados `NB_AUDIO_EVT_PLAYBACK_START/END` e integracao
  indireta com event bus via boot manager.
- `wake_service_rearm()` associado ao lifecycle legado de playback.
- Compatibilidade de playback local `PLAY_ACTIVE`, `PLAY_STOP`, synth/probe e
  silencio de fallback.
- VAD/turn-taking legado como rollback e fallback diagnostico.
- Ponte com `bridge_service` enquanto Capture v2/Codec v2 ainda precisam de
  rollback e handoff controlado.

Responsabilidades que ja pertencem majoritariamente aos v2:

- Fila e lifecycle de SAY: `audio_playback_service_v2`.
- Preparacao/commit/orquestracao do frame de SAY: `audio_playback_service_v2`
  por callback seguro do `audio_service`.
- Distribuicao/telemetria RX/TX, recoveries e heap: `audio_io_service_v2`.
- Decisao de fim de fala dentro de sessao aberta: `voice_activity_service_v2`.
- Estado de captura, ownership TX e envio quando armado: `voice_capture_session_v2`.
- Worker Opus, filas de codec, egress e rollback PCM16: `audio_codec_service_v2`.

Proximo incremento recomendado:

- P2 deve ser um corte de contrato/observabilidade, nao um novo handoff fisico:
  expor um mapa resumido de ownership em `/api/audio/voice-v2` ou endpoint
  dedicado antes de mover qualquer chamada HAL. O aceite deve provar que o
  firmware consegue dizer, em uma tela, quem e dono de RX, TX, VAD, captura,
  codec, playback e ponte legacy.

Nao fazer em P2:

- Nao mover `audio_hal_spk_write()` para Playback v2 ainda.
- Nao remover VAD legado.
- Nao trocar bridge TX sem rollback.
- Nao mexer em wake/follow-up/AEC.
