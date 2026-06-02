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

### N5 - Reduzir audio_service.c

Ultima etapa, apos donos reais validados:

- `audio_service.c` vira ponte/compatibilidade e orquestracao legada minima.
- Remover decisao duplicada apenas quando os servicos v2 ja tiverem gates e
  rollback equivalentes.
- Aceite: arquitetura em camadas preservada, nenhum comportamento v1 quebra, e
  o caminho default de voz em hardware passa por Capture v2, Activity v2,
  Audio IO v2 e Playback v2.

## Ordem Recomendada

1. Fase I: playback v2 como dono gradual do downlink.
2. Fase M parcial: checklist/health de release para proteger o que ja ficou
   bom.
3. Fase J: voice activity v2 em shadow/opt-in, sem AEC.
4. Fase K: capture session v2 assume upstream por flag.
5. Fase L: policy conversacional avancada, so depois de no-echo e captura
   estarem estaveis.
6. Fase N0-N5: migracao estrutural do firmware v2, com gates antes de cada
   troca de owner real.

Essa ordem segue a regra central da arquitetura v2: separar I/O, playback,
processor, codec e policy. No NoiseBot, a prioridade imediata e diminuir o
acoplamento do `audio_service.c` sem trocar wake, VAD, AEC e follow-up no mesmo
movimento.
