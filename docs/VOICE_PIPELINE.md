# Voice Pipeline

Este documento fixa o contrato de áudio conversacional do NoiseBot. A regra é
simples: firmware, bridge e server devem concordar nos mesmos limites antes de
qualquer otimização de STT/TTS.

## Contrato v1

- Formato: PCM16 little-endian, mono, 16 kHz.
- Chunk: 256 samples, 512 bytes, 16 ms.
- Modo de escuta: `auto`.
- Duração máxima de fala no firmware: 9,2 s.
- Áudio mínimo para STT: 8000 samples, 500 ms.
- Áudio máximo para STT no server: 192000 samples, 12 s.
- Silêncio final: 900 ms.
- Pre-roll no firmware: 320 ms.

O firmware anuncia esse contrato no `HELLO` do bridge e o server valida o
tamanho dos chunks recebidos. O server mantém folga de 12 s para absorver
atraso de `VOICE_END` e alinhamento por chunks sem descartar fala válida como
`audio_longo`.

## Decisões

- O caminho atual permanece PCM16 como fallback seguro. Opus 16 kHz mono em
frames de 60 ms foi fechado como transporte v2 local default do server, com
rollback operacional para PCM16 por env/restart ou `codec-v2
transport-disable`.
- Supressão de ruído em Python fica desligada por padrão. Nos testes práticos
ela piorou a transcrição e aumentou risco de watchdog.
- Backpressure persistente no bridge encerra a sessão. Áudio atrasado ou
  picotado não deve chegar ao STT como se fosse fala válida.
- O teto de 10 s é compartilhado entre firmware e server. Sessões longas são
  descartadas sem chamar STT.
- O server rechunkeia a saída TTS para frames exatos de 512 bytes antes de
  enviar `SAY`; chunks maiores vindos do gerador de áudio nunca cruzam o
  contrato TCP com o firmware.
- O `OutputScheduler` do server pode fazer um prebuffer curto, mas depois deve
  respeitar a cadencia de 16 ms por chunk. Pausas entre sentencas do TTS nao
  podem gerar rajadas de catch-up, porque isso enche a fila SAY do firmware e
  causa engasgos mesmo quando `tts_completed=true` e `SAY_END` foi enviado.

## Contrato v2

O NoiseBot usa Opus 16 kHz mono com frames de 60 ms, filas curtas e
processamento de voz condicionado às capacidades reais do hardware. O que esta
fixado agora e: Opus como codec negociado, capacidades explícitas no protocolo
e AEC como modo condicionado a referência limpa, não como feature universal do
ESP32-S3.

O plano completo para refazer o subsistema de voz de forma paralela e segura
esta em `docs/VOICE_AUDIO_V2_ARCHITECTURE.md`. Ele separa Audio I/O, playback,
captura de sessao, VAD/AFE, codec e bridge, com PCM16 como fallback e Opus
opt-in no firmware. As fases restantes pos-Opus estao em
`docs/VOICE_AUDIO_V2_NEXT_PHASES.md`.
O checklist/health de release da Fase M parcial fica em
`docs/VOICE_AUDIO_V2_RELEASE_CHECKLIST.md`.

Status v2 atual: Audio I/O e playback ja possuem probes explicitos validados.
Playback v2 tambem fechou o handoff parcial da Fase I pos-Opus no downlink SAY real:
`audio_playback_service_v2` agora e dono da fila estatica SAY de 16 chunks e
expoe `enqueue/dequeue/cancel/status`; `audio_service` continua dono do
speaker/HAL e drena essa fila pelo contrato v2. Os campos aparecem em
`/api/audio/playback-v2`, incluindo `bridge_say_queue_owner`, e preservam o
audio real pelo mesmo caminho de escrita no HAL. Validacao em hardware apos
flash confirmou 283 chunks SAY recebidos/tocados, fila final zero, zero drops,
`SAY_END` confirmado e `ESP_OK`. Uma rodada controlada pos-restart confirmou
tambem o barge-in real: `ww -> que horas sao?` respondeu como `local_time`, e
`ww -> me conte uma historia longa -> ww -> pare` registrou
`outcome=interrupted`, `discard_reason=barge_in`, cancelamento p50 2,6 ms /
p95 3,2 ms, fila final zero, `say_cancel_count=2`,
`say_chunks_cancelled=28` e `ESP_OK`.
Depois de uma resposta curta com 395 chunks TTS, o usuario percebeu engasgos e
os contadores mostraram `say_chunks_dropped` crescendo em 143 enquanto
`tts_completed=true` e `tts_say_end_sent=true`. A causa foi pacing server-side:
o `OutputScheduler` tentava compensar pausas de sintese entre sentencas com
rajadas SAY. O scheduler agora limita o prebuffer e envia chunks extras em
cadencia de 16 ms, sem catch-up agressivo. Validacao local: `server/tests`
com 155 testes verdes. Essa correcao e server-only e nao exige flash. Validacao
real apos restart do server: baseline `received=494`, `played=494`,
`dropped=154`; apos `ww -> me conte uma historia curta`, o status ficou
`received=892`, `played=892`, `dropped=154`, ou seja, +398 chunks recebidos e
tocados com zero drops novos. `/ai/metrics` confirmou `tts_completed=true`,
`tts_say_end_sent=true`, `tts_chunks_sent=398` e `voice_alert=null`.
Repeticoes fisicas posteriores separaram dois casos: uma rodada por wake teve
+222 chunks sem drops novos, mas transcript diferente do comando esperado; a
rodada seguinte ouviu `Me fala em historia curta.`, completou TTS e `SAY_END`,
mas Playback v2 acumulou +18 drops enquanto tocava +274 chunks. O ponto
amarelo atual e a transicao fisica wake -> resposta, nao o codec nem o caminho
controlado. O prebuffer padrao do `OutputScheduler` foi reduzido de 12 para 6
chunks via default de `NOISEBOT_TTS_QUEUE_TARGET`, deixando mais espaco livre
na fila SAY de 16 chunks antes de novo handoff.
A repeticao fisica apos esse ajuste validou o novo default: o turno
`Me diga uma fala com história curta.` enviou 326 chunks TTS completos, com
`SAY_END`, `voice_alert=null`, 3 paginas de texto visual completas e zero drops
novos em Playback v2 (`received=2569/played=2566/dropped=56` para
`received=2895/played=2892/dropped=56`). Codec v2 permaneceu saudavel e
Capture v2 desligado.
Resposta longa tambem passou com o prebuffer 6: 825 chunks TTS completos,
`SAY_END`, `voice_alert=null` e zero drops novos em Playback v2. Barge-in
fisico passou com `discard_reason=barge_in` e cancelamento em 3,3 ms; drops
novos ficaram restritos a `say_chunks_dropped_listening`, como descarte de
audio antigo durante a nova escuta. Ponto corrigido no firmware: ao receber
`SPEECH_CANCEL` ou `LISTEN_START`, o `behavior_engine` agora limpa o texto
visual antigo com `ui_overlay_clear_text()` antes de mostrar `Ouvindo...`.
Ponto corrigido no server apos validacao fisica: quando o usuario tenta
`ww -> pare` logo apos barge-in, o STT pode confundir o comando curto com
`Vale.` ou `Tchau.`. O `LocalIntentProvider` agora trata esses mishears como
`local_stop` somente dentro da janela curta de barge-in recente, respondendo
`Pronto, parei.` sem chamar a LLM; fora desse contexto, `Vale.` continua sem
ser forçado para stop e `Tchau.` segue como despedida normal. Validacao final:
apos `ww -> historia longa -> ww -> pare`, `/ai/metrics` registrou
`intent_name=local_stop`, transcript `Vale.`, `tts_completed=true`,
`tts_say_end_sent=true`, Playback v2 com fila zero e `codec-v2 health` ok apos
`egress-drain`.
`voice_capture_session_v2` possui replay/status/cancel via
`/api/audio/capture-v2` e acompanhamento PCM16 real atras da flag
`voice_audio_v2_capture_enabled`, desligada por padrao. Com a flag desligada, o
caminho v1 segue ativo. Com a flag ligada, o wake abre estado de sessao v2 e
contabiliza start/chunks/fim/cancelamento, enquanto o envio
`VOICE_START/AUDIO_CHUNK/VOICE_END` ao bridge permanece no caminho validado do
`audio_service`. O status HTTP de captura expõe `real_capture_enabled` para a
flag e `real_capture` para diferenciar replay de uma sessao PCM16 real.
Inicio da Fase K: o status tambem expoe `end_reason`, `bridge_tx_owner=false`
e `legacy_audio_service_tx_owner=true`, tornando observavel que o Capture v2
ainda nao assumiu o bridge TX real. Esse incremento e apenas contrato/status e
nao altera wake, VAD, codec, playback, bridge real ou HAL.
Validacao em hardware apos flash confirmou o contrato: replay diagnostico de
640 ms de fala + 900 ms de silencio terminou com
`end_reason=SPEECH_COMPLETE`, `voice_start_sent=true`,
`voice_audio_sent=true`, `voice_end_sent=true`, `captured_samples=10240`,
`dropped_frames=0`, `bridge_tx_owner=false` e
`legacy_audio_service_tx_owner=true`; Opus v2 foi reativado e `codec-v2
health` voltou ok.
O incremento local seguinte adiciona contadores shadow de TX ao mesmo status:
`shadow_voice_start_sent`, `shadow_voice_end_sent`, `shadow_audio_chunks`,
`shadow_audio_samples` e `shadow_audio_dropped_chunks`. Eles espelham o caminho
legado para dizer quando o Capture v2 emitiria os eventos/chunks, sem enviar
nada ao bridge e sem mudar ownership.
Validacao em hardware apos flash do shadow TX confirmou o replay diagnostico
com `shadow_audio_chunks=40`, `shadow_audio_samples=10240`,
`shadow_audio_dropped_chunks=0`, start/end shadow verdadeiros,
`bridge_tx_owner=false`; Opus v2 foi reativado e `codec-v2 health` ficou ok.
No primeiro turno real com a flag ligada, o comportamento ficou saudavel
(`voice_alert=null`, Playback v2 sem drops, Codec v2 ok), mas a telemetria
mostrou que, em Opus, `shadow_audio_samples` somava 256 samples por pacote
drenado. A correcao local faz o shadow somar 960 samples por pacote Opus
drenado, mantendo o TX real no `audio_service`.
Depois do flash dessa correcao, o reteste real alinhou
`shadow_audio_samples=52800` com `total_samples=52784` no server; a diferenca
de 16 samples e de alinhamento. Em seguida, o shadow foi refinado para fazer
`shadow_audio_chunks` e `speech_elapsed_ms` seguirem unidades/tempo de frame
Opus quando recebe pacotes de 960 samples.
Revalidacao final apos flash confirmou a telemetria Opus: no turno real
`Me diga uma curiosidade curta.`, `shadow_audio_chunks=58` bateu com
`chunk_count=58`, `shadow_audio_samples=55680` ficou alinhado com
`total_samples=55664`, `speech_elapsed_ms=3480`, Playback v2 teve zero drops e
Codec v2 ficou ok. Capture v2 permanece shadow/observador com
`bridge_tx_owner=false`.
O incremento local seguinte adiciona o gate de handoff no mesmo endpoint:
`bridge_tx_candidate`, `bridge_tx_handoff_ready` e `handoff_block_reason`.
Esses campos so classificam a sessao observada e nao transferem ownership. O
handoff so pode ser considerado pronto quando a sessao real terminou com
`SPEECH_COMPLETE`, teve start/end shadow, audio shadow nao vazio e zero drops;
caso contrario o status explica o bloqueio.
Validacao em hardware apos flash confirmou o gate: replay diagnostico nao vira
candidato (`NOT_REAL_CAPTURE`), enquanto um turno real por wake ficou
`bridge_tx_candidate=true`, `bridge_tx_handoff_ready=true` e
`handoff_block_reason=NONE`, com Playback v2 sem drops e Codec v2 saudavel.
Como o mesmo turno encerrou por timeout no server, repetir um turno curto com
fim por silencio antes de qualquer handoff real de TX.
Essa repeticao tambem passou: `ww -> que horas sao` fechou com
`voice_end_reason=silence`, transcript correto, gate verde, zero drops na
Capture v2 e Playback v2, TTS completo e `SAY_END`. Um pacote egress Opus
ficou pendente sem erro/drop e foi drenado; o health voltou `status=ok`.
O passo local seguinte prepara o armamento operacional do handoff real:
`voice_audio_v2_capture_tx_enabled` nasce desligada por padrao e aparece em
`/api/config`, `/api/config/all`, `/api/audio/capture-v2` e no CLI
`capture-v2 tx-enable|tx-disable`. Ela ainda nao muda o TX real; serve para o
proximo incremento poder ligar/desligar a troca de ownership sem reflash.
`GET /api/audio/codec-v2` expõe o contrato do codec v2 sem ativar worker,
bridge ou Opus como padrão: PCM16 default, Opus opt-in em 16 kHz mono,
60 ms/960 samples, 32 kbps e fila curta de 40 pacotes. Em hardware, após
flash, o endpoint retornou `initialized=false`, `format=pcm16`, contadores
zerados e `error=ESP_OK`; a captura v2 permaneceu desligada. O server também
expõe proxy em `/api/device/audio/codec-v2` e CLI
`noisebot_server debug codec-v2 status`. O teste sintético
`codec-v2 encode-test` exercita apenas PCM16 passthrough e contadores internos,
sem worker, sem Opus real e sem mudança no transporte; em hardware retornou
`pcm_frames_in=1`, `packets_out=1`, `packet_drops=0`, `queue_count=0` e
`pending_samples=64` com `ESP_OK`. O packetizer sintético já acumula chunks
PCM16 de 256 samples até formar frame de 960 samples, deixando 64 samples
pendentes quando recebe 4 chunks no teste. A fila sintética do codec v2 aceita
até 40 pacotes e passa a contar `packet_drops` quando esse limite é excedido,
ainda sem worker, sem Opus real e sem transporte para o bridge. O drain
sintetico em `codec-v2 drain` zera apenas a fila pronta e retorna
`drained_packets`, preservando amostras pendentes e contadores acumulados. Em
hardware, após flash, a sequência `status -> encode-test -> drain -> status`
confirmou `drained_packets=1`, `queue_count=0`, `pending_samples=64`,
`packet_drops=0` e `ESP_OK`; `capture-v2` permaneceu desligado. O reset
diagnostico em `codec-v2 reset` zera contadores, fila e amostras pendentes
sem alterar `format=pcm16` nem o contrato fixo do codec. Em hardware, após
flash, `encode-test -> reset -> status` confirmou contadores zerados,
`pending_samples=0`, `format=pcm16`, `ESP_OK` e `capture-v2` desligado.
O overflow-test diagnostico em `codec-v2 overflow-test --packets N` limpa o
estado antes e depois do teste, tenta enfileirar pacotes completos sinteticos e
retorna metricas separadas (`accepted_packets`, `dropped_packets`,
`peak_queue_count`) sem deixar drops acumulados no status global. Em hardware,
apos flash, `packets=40/41/45` retornaram respectivamente `0/1/5` drops,
sempre com `peak_queue_count=40`, limpeza final em zero e `ESP_OK`; o status
global do codec voltou limpo e `capture-v2` permaneceu desligado.
O status do Codec v2 agora explicita o worker opt-in:
`worker_supported=true`, `worker_active=false`, `worker_state=not_started` e
`worker_drained_packets`, alem dos contadores `worker_opus_*`. O boot nao cria
task. `codec-v2 worker-start` cria a task FreeRTOS `nb_codec_v2_worker` apenas
sob comando explicito; ela abre o encoder Opus no contexto do worker, consome a
fila sintetica, codifica um frame sintetico de 960 samples por pacote, soma
`worker_drained_packets` e atualiza `worker_opus_packets`,
`worker_opus_encoded_bytes_total` e `worker_opus_last_packet_bytes`.
`codec-v2 worker-stop` solicita parada, drena a fila restante e deixa
`worker_state=stopped`. Isso ainda nao liga bridge, captura ou playback v2; o
PCM16/v1 segue como padrao. A validacao local passou com teste focado de
contrato, `server/tests`, `bridge/tests` e `idf.py build`. Em hardware, antes
do Opus no worker, a sequencia
`status -> worker-start -> encode-test -> status -> worker-stop -> capture-v2`
confirmou `worker_supported=true`, worker rodando, `queue_count=0` apos drain
pela task, `worker_drained_packets=1`, `worker_state=stopped` no stop,
`packet_drops=0` e `capture-v2` desligado. Com Opus no worker, a mesma
sequencia em hardware confirmou `worker_opus_packets=1`,
`worker_opus_encoded_bytes_total=248`, `worker_opus_last_packet_bytes=248`,
`queue_count=0`, `packet_drops=0`, `error=ESP_OK`, `worker_state=stopped` e
`capture-v2` desligado. A primeira tentativa em hardware com stack persistente
de 24 KB em heap interno falhou no `worker-start` com HTTP 409 e
`worker_state=error`, sem derrubar HTTP. A tentativa com stack interna de 12 KB
criou a task, mas o encode deixou o HTTP indisponivel. A correcao local voltou
o worker para 24 KB, agora com stack em PSRAM via
`xTaskCreatePinnedToCoreWithCaps` e delecao por `vTaskDeleteWithCaps`;
teste focado/build passaram e a validacao em hardware fechou o contrato.
O novo `codec-v2 worker-stress-test --packets N` amplia esse passo sem ligar
captura real nem bridge: ele inicia o worker opt-in, enfileira ate 40 pacotes
sinteticos completos, espera a task codificar/drenar, para o worker e retorna
deltas de pacotes Opus, bytes, drops, fila final e estado final. A validacao
local passou com contrato bridge focado, server facade e `idf.py build`. Em
hardware apos flash, `--packets 10` retornou `worker_opus_packets_delta=10`,
`worker_opus_encoded_bytes_delta=2434`, `packet_drops_delta=0`,
`queue_count_after=0`, `worker_state_after=stopped` e `ESP_OK`; `capture-v2`
permaneceu desligado.
O novo `codec-v2 worker-feed-test --frames N` passa pelo caminho mais proximo
do codec real sem ligar captura nem bridge: ele inicia o worker opt-in,
alimenta frames PCM16 sinteticos de 960 samples via
`audio_codec_service_v2_feed_pcm16()`, deixa o packetizer formar pacotes,
espera a task codificar/drenar, para o worker e retorna deltas de frames PCM,
pacotes, bytes Opus, drops, fila final, pendencias e estado final. A validacao
local passou com contrato bridge focado, server facade e `idf.py build`. Em
hardware apos flash, `--frames 10` retornou `pcm_frames_in_delta=10`,
`packets_out_delta=10`, `worker_opus_packets_delta=10`,
`worker_opus_encoded_bytes_delta=2434`, `packet_drops_delta=0`,
`queue_count_after=0`, `pending_samples_after=0`, `worker_state_after=stopped`
e `ESP_OK`; `capture-v2` permaneceu desligado.
O worker Opus agora tem um observador diagnostico de payload: para cada pacote
Opus codificado, ele atualiza contadores de payload, bytes totais, sequencia,
checksum e preview fixa de ate 16 bytes do ultimo payload. Esses campos aparecem
no status do Codec v2 e no `worker-feed-test`, mas nao criam fila de rede, nao
enviam nada ao bridge e nao tocam captura/playback. A validacao local passou
com contrato bridge focado, server facade e `idf.py build`; a validacao em
hardware com `worker-feed-test --frames 10` retornou payload observado, preview
nao vazio, zero drops e `capture-v2` desligado.

O Codec v2 agora tambem possui uma fila egress Opus diagnostica e limitada a
40 pacotes. Ela representa o contrato interno "pacote Opus pronto para envio",
mas ainda nao envia nada ao `bridge_service`: o worker apenas registra
contadores, bytes, drops, checksum e preview do ultimo pacote. O endpoint
`codec-v2 egress-drain` limpa a fila, e `worker-feed-test` faz cleanup
automatico ao final. A validacao local passou com contrato bridge focado,
server facade e `idf.py build`; a validacao em hardware com
`worker-feed-test --frames 10` retornou 10 pacotes egress, 2434 bytes, zero
drops, cleanup da fila para zero e `capture-v2` desligado.
O stub diagnostico `codec-v2 bridge-handoff-test --frames N` e o primeiro
contrato de fronteira com a bridge: ele roda `feed_pcm16 -> worker Opus ->
egress`, registra `bridge_handoff_*` como pacotes prontos, mas retorna
`bridge_packet_not_sent=true` e `bridge_transport_unchanged=true`. Portanto
ele ainda nao envia Opus pelo bridge, nao renegocia `HELLO`, nao ativa
`bridge_service_set_opus_enabled()`, nao toca captura/playback e nao promove
Opus como padrao. A validacao local passou com teste focado bridge, teste
focado server e `idf.py build`. Em hardware apos flash,
`bridge-handoff-test --frames 10` retornou 10 pacotes prontos, 2434 bytes,
zero drops, `bridge_packet_not_sent=true`, `bridge_transport_unchanged=true`,
`opus_egress_queue_count_after_cleanup=0` e `worker_state_after=stopped`;
status final confirmou `format=pcm16`, `bridge_handoff_packets_ready=10`,
fila egress zerada, `error=ESP_OK` e `capture-v2` desligado.

O controle v2 para transporte Opus live existe como opt-in explicito:
`codec-v2 transport-enable` e `codec-v2 transport-disable`. No firmware, os
endpoints sao `/api/audio/codec-v2/transport/enable` e
`/api/audio/codec-v2/transport/disable`; no server, os proxies sao
`/api/device/audio/codec-v2/transport/enable` e
`/api/device/audio/codec-v2/transport/disable`. O CLI expõe:
`noisebot_server debug codec-v2 transport-enable --json` e
`noisebot_server debug codec-v2 transport-disable --json`. O transporte agora
usa o worker live do proprio `audio_codec_service_v2`: o endpoint de enable
inicia `audio_codec_service_v2_worker_start()`, liga
`bridge_service_set_opus_enabled(true)` e o loop do `audio_service` alimenta o
codec v2 com PCM16 normalizado. O worker v2 codifica frames reais de 960
samples e guarda pacotes Opus em uma fila egress real para
`audio_service` drenar com `audio_codec_service_v2_read_opus_packet()` antes
de chamar `bridge_service_send_opus_packet()`. O retorno marca
`codec_v2_transport=true`, `transport_worker="audio_codec_service_v2"`,
`compat_worker="audio_codec_service_v2"` e `pcm16_fallback=true`.
`transport-disable` desliga primeiro o Opus no bridge e para o worker v2,
mantendo rollback imediato para PCM16. O caminho PCM16 continua padrao; este
passo nao altera wake, VAD, state machine, follow-up, captura v2 ou playback
v2. Validacao local atual: contrato bridge focado, server facade e
`idf.py build` limpo. Validacao em hardware apos flash passou: status inicial
limpo, `transport-enable` retornou `transport_worker="audio_codec_service_v2"`,
`opus_enabled=true` e `ESP_OK`; status ativo mostrou worker `running`,
`opus_codec_error=0`, filas zeradas e zero drops. Um turno real com transcript
`Me conte uma história curta.` teve `transcript_quality=good`, `outcome=llm`,
`turn_id=4`, `chunk_count=39`, `total_samples=37424`, `duration_ms=2339.0`,
`stt_ms=1088.0`, `first_audio_out_ms=5480.9` e resposta LLM falada. O worker
v2 registrou `pcm_frames_in=39`, `worker_opus_packets=39`,
`worker_opus_encoded_bytes_total=9488`, `opus_egress_packets_drained=39`,
`packet_drops=0`, `opus_egress_packet_drops=0`, `queue_count=0` e
`opus_codec_error=0`. `transport-disable` retornou
`live_bridge_transport=false`, `opus_enabled=false` e `ESP_OK`; status final
ficou com `worker_active=false`, `worker_state=stopped`, fila zero e PCM16
como fallback.
O `codec-ab` curto agora usa o transporte `codec-v2` em vez do endpoint Opus
legado. Em hardware, com a frase pareada `me diga uma curiosidade`, o PCM16
retornou `ok=true`, `turn_id=7`, `outcome=local_intent`,
`transcript_quality=good`, transcript `Diga uma curiosidade.`,
`transcript_similarity=0.858`, `total_samples=51200`, `duration_ms=3200.0` e
`stt_ms=1060.0`. O Opus v2 retornou `ok=true`, `turn_id=8`,
`outcome=local_intent`, `transcript_quality=good`, transcript
`Me diga uma curiosidade.`, `transcript_similarity=1.0`,
`total_samples=83504`, `duration_ms=5219.0`, `stt_ms=1107.8`,
`packets_drained=87`, `packet_drops=0`, `encoded_bytes=21368` e
`server_codec_confirmed=true`. Apos o A/B, o status do Codec v2 confirmou
worker parado e `packet_drops=0`; uma sobra de 1 pacote egress foi drenada
por `codec-v2 egress-drain`. Para fechar esse rollback automaticamente, o
`transport-disable` local agora drena a fila egress e retorna
`egress_drained_packets`; o harness `codec-ab` tambem chama `egress-drain`
apos desabilitar Opus.
Depois do flash da correcao de rollback, a bateria `codec-ab --repeat 3` com
a mesma frase pareada tambem passou. PCM16: 3/3 `ok=true`, todos com
`transcript_quality=good`, match semantico e STT medio ~1081.0 ms. Opus v2:
3/3 `ok=true`, todos com transcript `Me diga uma curiosidade.`,
`transcript_similarity=1.0`, STT medio ~1086.6 ms, `packet_drops=0`,
334 pacotes drenados pelo harness e 81213 bytes Opus. O status final do Codec
v2 confirmou worker `stopped`, `packet_drops=0`,
`opus_egress_packet_drops=0`, `opus_egress_queue_count=0`,
`opus_codec_error=0` e PCM16 como fallback. Decisao: Opus v2 e candidato
forte, mas segue opt-in ate validar regressao de barge/no-echo e decidir
HELLO/capability oficial.
Os harnesses `barge-live` e `no-echo-live` agora aceitam
`--codec pcm16|opus-v2` e `--firmware-url`/`--host`. Em `opus-v2`, eles usam
`/api/audio/codec-v2/transport/enable` antes do turno, coletam contadores do
Codec v2, validam que houve pacotes Opus drenados sem drops e fazem rollback
automatico com `transport-disable` seguido de `egress-drain`. A validacao local
do server passou com `server/tests` completo. A validacao em hardware tambem
passou:

- `barge-live --codec opus-v2`: `ok=true`, `interrupted_turn_id=25`,
  `interruption_cancel_ms=1.6`, `discard_reason=barge_in`,
  `outcome=interrupted`, 137 pacotes Opus drenados, 33558 bytes,
  `packet_drops=0`, `enable_ok=true`, `disable_ok=true` e
  `server_codec_confirmed=true`.
- `no-echo-live --codec opus-v2`: `ok=true`, `response_turn_id=27`,
  `unexpected_turn_id=null`, janela silenciosa de 10s, `outcome=llm`,
  56 pacotes Opus drenados, 13856 bytes, `packet_drops=0`, `enable_ok=true`,
  `disable_ok=true` e `server_codec_confirmed=true`.

Decisao: os bloqueios de barge/no-echo para Opus v2 opt-in estao verdes e a
migracao Opus v2 foi fechada como default local do server, mantendo rollback
PCM16.
O primeiro Opus real do Codec v2 entrou como diagnóstico isolado em
`codec-v2 opus-encode-test`: o firmware cria uma task temporaria com stack
proprio, abre o encoder Opus da Espressif, codifica um frame sintético de 960
samples, fecha o encoder e retorna `encoded_bytes`, heap e `codec_error`. Ele
não cria worker persistente, não envia ao bridge, não toca captura/playback e
não muda o padrão PCM16. A tentativa inicial síncrona no handler HTTP causou
timeout/indisponibilidade HTTP no hardware; por isso o teste passou a rodar em
task temporária. A validação local passou com contrato bridge focado, server
facade, `bridge/tests`, `server/tests` e `idf.py build`. Em hardware apos
flash, o teste corrigido retornou `encoded_bytes=248`, `codec_error=0`,
`opus_encode_tests=1`, `queue_count=0`, `packet_drops=0`,
`worker_active=false`, `worker_state=stopped` e `ESP_OK`; o status seguinte
confirmou HTTP saudavel e `capture-v2` permaneceu desligado.

A nota de consulta para Obsidian/IA fica em
`docs/OBSIDIAN_VOICE_AUDIO_V2_KNOWLEDGE.md`, com decisoes, parametros,
comandos, riscos e perguntas obrigatorias antes de mexer no subsistema de voz.

## Roadmap de Fechamento do Ciclo

O objetivo deste ciclo é transformar voz em uma parte confiável do produto:
acordar, ouvir, entender, responder e voltar ao estado base sem loops, travas,
latência excessiva ou regressão em câmera/TTS.

### Fase 1 — Contrato Estrito de Voz

Status: concluída.

Mudanças:

- Firmware e server anunciam o mesmo contrato de áudio no `HELLO`.
- O teto de fala é 10 s no firmware e no server.
- O server rejeita chunk fora do tamanho contratado.
- Backpressure persistente encerra sessão em vez de enviar áudio picotado.
- Sessões acima do teto são descartadas antes do STT.

Critérios de aceite:

- Build ESP-IDF sem warnings.
- Testes do server verdes.
- Pergunta curta por voz gera uma única transcrição e resposta.
- Silêncio após wake não entra em loop de prompts.
- Bridge congestionado não gera STT com áudio atrasado.

### Fase 2 — Métricas e Observabilidade de Voz

Status: concluída no server/dashboard. O resumo da última sessão, o histórico
recente, os alertas de descarte/falha e o diagnóstico acionável já são
registrados no server, expostos em `/ai/metrics` e exibidos no dashboard dev.

Objetivo: saber exatamente onde está a latência ou falha.

Mudanças:

- Expor no dashboard dev:
  - duração da última fala;
  - motivo do fim da sessão;
  - chunks enviados, chunks dropados e primeiro áudio;
  - tempo `VOICE_END -> STT`;
  - tempo `STT -> primeiro TTS`;
  - tempo `primeiro TTS -> áudio no firmware`;
  - qualidade STT e motivo de descarte.
- Expor em `/ai/metrics` o resumo da última sessão de voz.
- Expor em `/ai/metrics` o histórico recente de sessões.
- Registrar eventos de sessão em log estruturado no server.
- Mostrar alerta quando o áudio for descartado por `audio_longo`, `audio_curto`,
  `backpressure` ou `chunk_invalido`.

Critérios de aceite:

- Dashboard explica uma falha de voz sem precisar abrir log bruto.
- Toda sessão tem começo, fim, duração, decisão de STT e decisão de TTS.
- Nenhum erro fica apenas como `warning` solto no terminal.

### Fase 3 — Turn-Taking Robusto

Status: concluída para turn-taking half-duplex; barge-in full-duplex fica
experimental até AEC. O barge-in agora registra o turno antigo como
interrompido, remove frames de fala pendentes da fila TCP antes do
`SPEECH_CANCEL`, tolera falha de cancelamento do firmware e abre o próximo turno
com watchdog ativo. O follow-up deixou de depender de `?` no texto exibido: o
server manda `FOLLOWUP_ARM` por `SESSION` quando a resposta real pede
continuação, e o prompt de wake vazio não arma nova escuta. Os demais
encerramentos agora usam contrato terminal explícito (`SESSION_DONE`,
`SESSION_ERROR` ou `FOLLOWUP_CANCEL`) para não deixar estado pendente no
firmware. O prompt de wake vazio permanece limitado a uma vez por sequência até
existir fala útil. O firmware também formaliza os modos `auto`, `manual` e
`realtime` na API de áudio; nesta fase apenas `auto` é suportado, enquanto
`manual` e `realtime` retornam `ESP_ERR_NOT_SUPPORTED`.
Validação em bancada mostrou que a interrupção durante TTS estava caindo no
follow-up pós-fala: o firmware não anunciava `barge_in`, não tratava
`SPEECH_CANCEL` e `audio_play_stop()` não encerrava `PLAY_BRIDGE_SAY`. O caminho
foi corrigido no protocolo: o firmware aceita `SPEECH_CANCEL` e drena a fila
SAY. A tentativa de disparar barge-in automaticamente por VAD secundário durante
TTS gerou falso positivo com o próprio áudio do robô; por isso o disparo
automático fica desativado até existir AEC/AFE validado.
Validação em hardware em 2026-05-29 confirmou o caminho suportado: wake word
durante `RESPONDING` interrompe o TTS, envia `SPEECH_CANCEL`, abre escuta
`source=barge_in`, encerra por silêncio e responde ao novo comando. A regressão
automática `bridge/tests/test_firmware_audio_service_contract.py` trava o
contrato mínimo no fonte do firmware para não voltar a forçar `VAD_ACTIVE` no
barge-in nem alterar a escuta normal por wake word.

Objetivo: o robô deve saber quando ouvir, quando parar e quando não responder.

Mudanças:

- Formalizar modos de escuta:
  - `auto`: wake word abre uma janela e encerra por silêncio;
  - `manual`: toque/botão mantém aberto até comando explícito;
  - `realtime`: reservado para futuro com AEC.
- Unificar as regras de wake, follow-up e silêncio.
- Garantir prompt de ajuda somente uma vez por wake sem fala.
- Fazer barge-in cancelar TTS sem deixar fila velha tocando.

Critérios de aceite:

- Wake sem fala: no máximo uma ajuda, depois volta ao `IDLE`.
- Wake + fala curta: uma resposta, sem repetir turno.
- Interromper TTS por wake word durante a fala funciona; interrupção automática
  por fala sem wake fica reservada para a fase com AEC/AFE validado.
- Follow-up funciona sem reacordar o robô artificialmente.

### Fase 4 — Qualidade de Entrada Sem Denoise Arriscado

Status: concluída como baseline inicial de diagnóstico. O firmware expõe
`POST /api/audio/record` para gravar WAVs diagnósticos no SD em `raw` ou
`bridge_tx`, e o server inclui análise local de WAV com RMS, pico, clipping e
duração. As primeiras amostras reais mostraram `bridge_tx` sem clipping e em
nível suficiente; o ganho não é o gargalo principal. O ajuste inicial foi mover
o Whisper para `beam_size=5` e usar `NOISEBOT_WHISPER_INITIAL_PROMPT` com
vocabulário curto do Noisebot, porque isso corrigiu as transcrições dos
comandos gravados sem aplicar denoise. Quando o Whisper marca `NO_SPEECH`, o
server zera o texto retornado para evitar vazamento do prompt inicial em
silêncio/follow-up vazio. O comando
`noisebot_server debug audio-report <pasta>` gera o baseline Markdown/JSON da
coleta; o relatório inicial está em `docs/VOICE_SAMPLES_PHASE4.md`.

Resultado de bancada em 2026-05-27:

- 17 WAVs analisados: `bridge_tx=9`, `raw=8`.
- Cenários: fala normal, fala baixa, fala longe, comando curto, ruído ambiente,
  mesa vibrando e probe curto.
- Clipping máximo observado: `0.0000%`.
- RMS médio: `bridge_tx=1842.28`, `raw=998.94`.
- Decisão: não ativar denoise Python; manter ganho/AGC atual e usar os arquivos
  gravados como baseline antes/depois para qualquer filtro futuro.

Objetivo: melhorar entendimento sem filtros que pioram a fala.

Mudanças:

- Manter denoise Python desligado por padrão.
- Criar gravação comparativa local:
  - áudio cru;
  - áudio enviado ao STT;
  - métricas RMS, peak, clipping e duração.
- Ajustar ganho de envio ao STT com base em dados reais.
- Detectar saturação/clipping e avisar no dashboard.
- Avaliar microfone, distância e ruído com amostras salvas.

Coleta inicial:

- `POST /api/audio/record` com `{"source":"raw","scenario":"fala_baixa","duration_s":5}`
  grava o microfone condicionado em `/sdcard/logs/audio/`.
- `POST /api/audio/record` com `{"source":"bridge_tx","scenario":"fala_baixa","duration_s":5}`
  grava o PCM que seria enviado ao STT.
- Repetir cada cenário em par `raw`/`bridge_tx`: fala normal, fala baixa,
  fala longe, ruído ambiente, mesa com vibração e comando curto.
- Copiar os WAVs do SD para o PC e analisar com
  `noisebot_server debug audio-report ..\voice_samples --output ..\docs\VOICE_SAMPLES_PHASE4.md`.

Critérios de aceite:

- Baseline inicial gravado e comparado sem clipping.
- Taxa de comandos entendidos melhorou com `beam_size=5` e prompt inicial sem
  ativar denoise.
- Nenhum filtro novo entra sem gravação antes/depois.
- O alvo original de 20 comandos continua recomendado para regressão, mas não
  bloqueia a transição para AFE porque o gargalo medido não foi ganho/clipping.

### Fase 5 — Pipeline AFE no Firmware

Status: experimento opt-in avançado para fonte processada do bridge; AFE é
candidata, ainda não é padrão. O
componente `audio_processor_service` continua desligado por padrão: sem endpoint
ativo ele não cria AFE, não recebe áudio e não altera o caminho
`audio_service -> bridge`. Quando acionado manualmente, ele cria uma instância
`AFE_TYPE_VC` em `AFE_MODE_HIGH_PERF`, recebe o mesmo PCM condicionado do
`audio_service`, mantém métricas de feed/fetch e pode expor a saída AFE como
fonte preferencial do bridge com fallback imediato para o PCM original.

Endpoints de bancada:

- Firmware direto: `GET /api/audio/processor`.
- Firmware direto: `POST /api/audio/processor/probe`.
- Firmware direto: `POST /api/audio/processor/shadow/start`.
- Firmware direto: `POST /api/audio/processor/shadow/stop`.
- Firmware direto: `POST /api/audio/processor/bridge/start`.
- Firmware direto: `POST /api/audio/processor/bridge/stop`.
- Via server: `GET /api/device/audio/processor`.
- Via server: `POST /api/device/audio/processor/probe` com token ops.
- Via server: `POST /api/device/audio/processor/shadow/start` com token ops.
- Via server: `POST /api/device/audio/processor/shadow/stop` com token ops.
- Via server: `POST /api/device/audio/processor/bridge/start` com token ops.
- Via server: `POST /api/device/audio/processor/bridge/stop` com token ops.

Primeiro probe em hardware, após flash de 2026-05-27:

- Boot normal confirmou `initialized=true`, `enabled=false`, `probe_ran=false`.
- `POST /api/audio/processor/probe`: `probe_ok=true`.
- PSRAM antes: 7337 KB.
- PSRAM após criar AFE VC: 7234 KB.
- PSRAM após destruir AFE VC: 7337 KB.
- Custo observado do probe: ~103 KB de PSRAM, devolvidos após destroy.
- IO do AFE VC: `feed_chunksize=160`, `fetch_chunksize=512`,
  `feed_channels=1`, `fetch_channels=1`, `sample_rate_hz=16000`.
- Health depois do probe: firmware seguiu responsivo, SD montado e sem queda de
  heap PSRAM livre.

Correção de RAW baseline em 2026-05-27:

- O caminho RAW deixou de usar `vad_process_with_trigger()` como juiz contínuo
  de fala/silêncio e passou a usar `vad_process()` para evitar sessões presas
  até o teto de 10 s.
- O teto local do firmware foi ajustado para encerrar antes do limite exato do
  server, reduzindo descarte por `audio_longo`.
- Teste manual pós-flash: turnos consecutivos encerraram por
  `voice_end_reason=silence` sem `audio_longo`.

A/B curto RAW vs AFE em 2026-05-27 (`docs/VOICE_AB_PHASE5_8192.md`):

- RAW bom: `1/2`.
- AFE bom: `2/2`.
- `no_speech` médio RAW: `0.358`.
- `no_speech` médio AFE: `0.049`.
- Fallbacks AFE: `35`.
- Overruns AFE: `0`.
- Decisão: AFE candidata; coletar mais repetições com frases variadas antes de
  promover como padrão.

A/B controlado RAW vs AFE em 2026-05-28:

- Firmware AFE opt-in subiu em runtime com `processed_bridge_enabled=true`.
- Custo observado do shadow AFE: ~128 KB de PSRAM.
- Rodada AFE: `processed_bridge_chunks=3922`, `fallbacks=36`,
  `overruns=0`, `shadow_fetch_nulls=0`.
- STT AFE: 3/3 `GOOD`, `no_speech` médio `0.007`, `logprob` médio `-0.54`.
- STT RAW: 3/3 `GOOD`, `no_speech` médio `0.007`, `logprob` médio `-0.37`.
- Decisão: AFE aprovado como experimento opt-in e saudável, mas ainda não
  promovido para padrão porque o RAW teve confiança STT melhor nesta rodada.
  Manter RAW como padrão e repetir A/B maior antes de qualquer promoção.

Guard de idioma em 2026-05-27:

- O prompt da LLM exige resposta em português do Brasil.
- O server aplica guard pós-LLM para substituir respostas com scripts
  chinês/japonês/coreano por fallback em português antes de TTS/robô.
- Teste prático com piadas/história confirmou respostas em português; um turno
  longo ainda encerrou por `timeout`, então a instabilidade remanescente é de
  turn-taking/entrada, não de idioma.

Correção de timeout pós-barge-in em 2026-05-27:

- Quando o usuário interrompe TTS, o server abre novo turno em `LISTENING`.
- Se nenhuma nova fala útil chegar até o watchdog, o turno agora encerra como
  `listen_timeout`, envia `FOLLOWUP_CANCEL` e retorna ao baseline sem
  `SESSION_ERROR`.
- Esse ajuste cobre o caso observado em que a fala era interrompida, mas a
  conversa parecia falhar depois por ausência de áudio no turno seguinte.

Objetivo: evoluir o processamento de voz no ESP,
mas sem sacrificar câmera, TTS e estabilidade.

Mudanças:

- Criar componente experimental `audio_processor_service` desligado por padrão.
- Avaliar AFE `AFE_TYPE_VC` em modo high performance por probe NVS.
- Testar VADN/NSNET se modelos estiverem disponíveis.
- Medir RAM com:
  - câmera ativa;
  - TTS ativo;
  - dashboard conectado;
  - bridge conectado;
  - SD logging ativo.
- Manter fallback para o VAD atual.

Sequência de ativação:

1. Build com o componente presente e `voice_afe_probe` desligado.
2. Boot normal: confirmar que logs mostram probe desabilitado e voz atual segue
   intacta.
3. Ligar `nb_svc/voice_afe_probe=1` em bancada.
4. Boot com WakeNet ativo: coletar PSRAM pré/pós `AFE_TYPE_VC`.
5. Só se a memória ficar confortável, criar etapa seguinte de feed/fetch
   espelhado sem enviar saída ao STT.
6. Só depois comparar WAV `bridge_tx` atual vs saída AFE em amostras pareadas.

Shadow mode atual:

- `audio_service` chama `audio_processor_service_feed_shadow()` com o mesmo PCM
  condicionado que alimenta sound analysis e bridge.
- O AFE VC recebe feed em paralelo, uma task própria faz fetch e guarda a saída
  em um ring buffer pequeno alocado em PSRAM.
- Métricas expostas: `shadow_feed_chunks`, `shadow_fetch_chunks`,
  `shadow_fetch_nulls`, `shadow_output_rms`, `shadow_output_peak`,
  `processed_buffer_level`, `processed_output_overruns` e PSRAM atual.
- A saída AFE ainda não altera `VOICE_START` nem `VOICE_END`.

Fonte processada do bridge:

- `POST /api/audio/processor/bridge/start` inicia o shadow se necessário e
  habilita `processed_bridge_enabled`.
- Durante uma sessão de escuta, `audio_service` tenta ler um chunk AFE de mesmo
  tamanho do chunk do microfone. Se houver dados suficientes, esse chunk vira a
  fonte enviada ao bridge/STT; se não houver, o chunk bruto condicionado segue
  imediatamente pelo caminho antigo.
- O ring buffer processado só aceita saída AFE entre `VOICE_START` e fim da
  sessão. Isso evita áudio antigo de idle/silêncio e mantém os primeiros chunks
  com fallback limpo quando a AFE ainda está atrasada.
- O AGC/limitador atual do `bridge_tx` continua aplicado depois da seleção da
  fonte, mantendo o contrato de amplitude do servidor.
- Métricas de segurança: `processed_bridge_chunks`,
  `processed_bridge_fallbacks`, `processed_output_overruns` e
  `processed_buffer_level`.

Critérios de aceite:

- Sem regressão de câmera 640x480.
- Sem queda do TTS.
- Sem watchdog em 30 minutos de uso misto.
- Ganho real de STT comprovado por amostras comparáveis.

Pendências para promover AFE:

- Rodar bateria A/B maior com 5 a 10 frases diferentes.
- Exigir zero overrun e fallback baixo/estável.
- Exigir ganho claro de STT contra RAW, não apenas estabilidade do pipeline.
- Confirmar que respostas longas não aumentam `timeout` ou `audio_longo`.
- Só então ligar fonte AFE como padrão do bridge, mantendo fallback RAW.

### Fase 6 — Opus/Frames de 60 ms

Status: concluida como capability oficial opt-in e candidata a A/B mais amplo.
PCM16 continua sendo o fallback seguro (`audio.format=pcm16`,
`pcm16=true`, `opus=false` quando Opus não é negociado). O firmware agora
consegue iniciar um worker Opus persistente, codificar PCM real em frames de
60 ms (`NB_BRIDGE_OPUS_FRAME_MS=60`, `NB_BRIDGE_OPUS_FRAME_SAMPLES=960`),
enfileirar pacotes Opus em PSRAM e, quando a flag opt-in é ligada,
anunciar `audio.format=opus` no `HELLO` e enviar esses pacotes como
`AUDIO_CHUNK`. O rollback é imediato via API: desligar a flag volta o contrato
para PCM16 e para o worker.

O server possui `noisebot_server.internal.transport.opus_codec` para round-trip
PCM16 -> Opus -> PCM16 com frames de 60 ms e comando
`noisebot_server debug opus-selftest`. O comando
`noisebot_server debug opus-live` automatiza a validacao real: liga Opus no
firmware, espera um turno novo em `/ai/metrics`, confere pacotes drenados e
desliga Opus no final. O adapter do server aceita um peer
experimental que negocie `audio.format=opus` e publica PCM16 para o
orchestrator. O fake firmware do server aceita `--audio-format opus`, anuncia
`codecs.opus=true`, empacota PCM em Opus e exercita o caminho TCP completo.
O `/ai/status` agora expõe `audio`, `codecs`, `features` e `firmware.*`, então
o harness `opus-live` consegue confirmar `opus_tx` depois da renegociação.

Objetivo: reduzir banda e aproximar o protocolo do NoiseBot quando fizer sentido.

Mudanças:

- Adicionar negociação de codec no `HELLO`:
  - `pcm16` como baseline;
  - `opus` como opcional.
- Implementar Opus apenas atrás de feature flag:
  - `POST /api/audio/opus/transport/enable`;
  - `POST /api/audio/opus/transport/disable`.
- Server aceita os dois formatos durante a transição.
- Medir latência e CPU antes de tornar padrão.

Validação atual:

- Firmware real, teste manual em 2026-05-29:
  - `POST /api/audio/opus/transport/enable` retornou `opus_enabled=true`;
  - `pcm_feed_chunks=519`, `pcm_feed_frames=138`, `pcm_feed_drops=0`;
  - `pcm_encode_packets=138`, `opus_packet_enqueued=138`;
  - `opus_packet_drained=138`, `opus_packet_drops=0`;
  - `opus_packet_queue_count=0`;
  - `POST /api/audio/opus/transport/disable` retornou `opus_enabled=false`.
- Firmware real, `opus-live` em 2026-05-29:
  - `ok=true`, `outcome=llm`, `transcript_quality=good`;
  - transcript: `Me diga uma curiosidade.`;
  - `total_samples=55664`, `duration_ms=3479.0`, `stt_ms=1088.6`;
  - `packets_drained=58`, `packet_drops=0`, `encoded_bytes=8357`;
  - `enable_ok=true`, `disable_ok=true`.
- Firmware real, `opus-live` multi-turn em 2026-05-29:
  - `me conte uma historia curta`: `ok=true`, `outcome=llm`,
    `total_samples=62384`, `packets_drained=65`, `packet_drops=0`;
  - `me diga outra curiosidade`: `ok=true`, `outcome=llm`,
    `total_samples=151664`, `packets_drained=158`, `packet_drops=0`;
  - `que horas sao`: `ok=true`, `outcome=local_intent`,
    `total_samples=46064`, `packets_drained=48`, `packet_drops=0`;
  - todos com `transcript_quality=good`, `enable_ok=true` e `disable_ok=true`.
- Firmware real, Opus live pelo namespace Codec v2 em 2026-05-31:
  - server rodando com `NOISEBOT_LLM_MODEL=qwen3.5:9b`;
  - `codec-v2 transport-enable` retornou `live_bridge_transport=true`,
    `opus_enabled=true`, `pcm16_fallback=true`, `ESP_OK`;
  - turno real: transcript `Fale uma frase curta.`, `transcript_quality=good`,
    `outcome=llm`, reply `Ola! Sou o NoiseBot e estou ansioso para conversar
    com voce.`;
  - `total_samples=51824`, `duration_ms=3239.0`, `stt_ms=1094.3`,
    `first_audio_out_ms=5490.9`, `tts_first_audio_ms=471.1`;
  - worker Opus de compatibilidade: `pcm_feed_frames=54`,
    `pcm_encode_packets=54`, `opus_packet_enqueued=54`,
    `opus_packet_drained=54`, `opus_packet_drops=0`,
    `opus_packet_queue_count=0`, `opus_packet_bytes_total=13110`,
    `codec_error=0`;
  - rollback: `codec-v2 transport-disable` retornou
    `live_bridge_transport=false`, `opus_enabled=false`, `ESP_OK`; status final
    confirmou `capture-v2` desligado e Codec v2 limpo em `format=pcm16`.
- Firmware real, `codec-ab` curto em 2026-05-29:
  - PCM16: 3/3 turnos `ok=true`, `transcript_quality=good`, STT medio
    1049,6 ms;
  - Opus: 3/3 turnos `ok=true`, `transcript_quality=good`, STT medio
    1089,0 ms;
  - Opus drenou 160 pacotes, 23045 bytes e `packet_drops=0`;
  - duas transcricoes Opus ficaram semanticamente piores que PCM16, apesar de
    aceitas pelo STT; decisao: Opus segue candidato opt-in e ainda nao vira
    padrao obrigatorio.
- Firmware real, `codec-ab --repeat 2` em 2026-05-29:
  - PCM16: 10/10 turnos `ok=true`, `transcript_quality=good`;
  - Opus: 10/10 turnos `ok=true`, `transcript_quality=good`;
  - Opus drenou 897 pacotes, 129696 bytes e `packet_drops=0`;
  - STT medio: PCM16 1412,0 ms, Opus 1420,7 ms;
  - match semantico estimado: PCM16 9/10, Opus 6/10;
  - decisao: transporte Opus aprovado como opt-in estavel, mas nao promover
    para padrao obrigatorio antes de melhorar/entender a perda semantica.
- Firmware real, teste cirurgico de curiosidade em 2026-05-29:
  - PCM16 ouviu `Anote uma curiosidade.`, marcou `ok=false` por similaridade
    literal (`0.707`), mas roteou `outcome=local_intent`;
  - Opus ouviu `Me conte uma curiosidade.`, `ok=true`, `outcome=local_intent`,
    `packets_drained=63`, `packet_drops=0`;
  - o problema de resposta em ingles para curiosidade ficou coberto por intent
    local e fallback pt-BR especifico.
- Diagnóstico offline em 2026-05-30 sobre 17 WAVs de `voice_samples`:
  - 16000 bps: compressão média `0.0519`, SNR médio `10.97 dB`,
    correlação média `0.9587`;
  - 24000 bps: compressão média `0.0804`, SNR médio `14.73 dB`,
    correlação média `0.9835`;
  - 32000 bps: compressão média `0.1080`, SNR médio `17.57 dB`,
    correlação média `0.9915`;
  - leitura: o codec em si preserva bem o sinal capturado, principalmente em
    24/32 kbps; a perda semântica vista no A/B live provavelmente está mais
    ligada a janela de captura/VAD/tempo de fala ou volume do teste do que a
    corrupção básica do Opus.
- Perfil adotado para o próximo teste live: manter Opus 16 kHz mono com frame
  de 60 ms, mas fixar o encoder do firmware em
  32 kbps (`OPUS_TARGET_BITRATE=32000`) porque foi o melhor resultado offline
  nas amostras reais do NoiseBot. PCM16 continua sendo o padrão seguro; Opus
  continua opt-in por API.
- Firmware real mantém PCM16 como padrão; Opus só liga por API experimental.
- Firmware real passou por builds ESP-IDF limpos após os commits:
  - `38eadb6` worker isolado;
  - `6715dc8` worker persistente;
  - `80ab9d6` teste de fila;
  - `2e20116`, `f9ffb42`, `3814e49` espelhamento PCM e fila;
  - `dc376f1` fila de pacotes Opus;
  - `243a7ca` envio ao bridge preparado;
  - `6fe5aa3` transporte Opus experimental.
- `server/tests/test_opus_codec.py`: frame Opus de 60 ms, packetizer e
  round-trip com compressão.
- `server/tests/test_server_facade.py`: adapter converte `AUDIO_CHUNK` Opus
  negociado para `AudioChunkIn` PCM16.
- `server/tests/test_server_facade.py`: fake firmware Opus via TCP chega ao
  orchestrator e aciona STT com PCM decodificado.
- `noisebot_server debug opus-live`: harness para teste real fim-a-fim com
  firmware, server metrics e desligamento automatico do transporte Opus.
- `noisebot_server debug codec-ab`: harness pareado para rodar PCM16 e Opus
  nas mesmas frases, com relatório Markdown/JSON de STT, duração, samples,
  pacotes, bytes, drops e similaridade semantica contra a frase alvo.
- `noisebot_server debug opus-quality`: diagnóstico offline para WAVs
  capturados, comparando PCM16 original contra round-trip PCM16 -> Opus ->
  PCM16 em múltiplos bitrates. O relatório atual fica em
  `docs/VOICE_OPUS_QUALITY.md`.
- Server Ops API proxy para os endpoints Opus do firmware:
  `/api/device/audio/opus/worker`,
  `/api/device/audio/opus/worker/probe`,
  `/api/device/audio/opus/worker/start`,
  `/api/device/audio/opus/worker/stop`,
  `/api/device/audio/opus/worker/encode-test`,
  `/api/device/audio/opus/worker/drain-packets`,
  `/api/device/audio/opus/transport/enable` e
  `/api/device/audio/opus/transport/disable`.
- Server Ops API proxy para os endpoints Capture v2 do firmware:
  `/api/device/audio/capture-v2`,
  `/api/device/audio/capture-v2/replay` e
  `/api/device/audio/capture-v2/cancel`.
- `noisebot_server debug capture-v2`: ferramenta manual para consultar status,
  executar replay sintetico, cancelar a captura v2 e rodar validacao `live`
  com rollback automatico da flag.
- `bridge/tests/test_firmware_bridge_contract.py`: firmware real mantém PCM16
  como contrato padrão e exige flag explicita para Opus.
- `bridge/tests`: 153 testes verdes após a integração.
- `server/tests`: 107 testes verdes após adicionar match semantico ao
  `codec-ab`.
- `server/tests`: 112 testes verdes após adicionar `opus-quality`.
- Build ESP-IDF concluído após a promoção de status/API.
- `noisebot_server debug opus-selftest --json`: 1s PCM16 16 kHz gerou 17
  packets, `opus_bytes=3043` contra `input_bytes=32000`
  (`compression_ratio=0.0951`) no ambiente local.

Critérios de aceite:

- [x] PCM continua funcionando como fallback e padrão.
- [x] Opus só liga por flag experimental.
- [x] Pacotes Opus reais são codificados, enfileirados, enviados/drenados sem
  drops no teste manual.
- [x] Filas continuam curtas e previsíveis (`queue_count=0` após envio).
- [x] Nenhuma mudança de codec quebrou a suíte do bridge.
- [x] Sessão real em Opus com STT `good` e resposta LLM ponta a ponta.
- [x] Sessão multi-turn em Opus com STT `good`, LLM/local intent e zero drops.
- [x] Manter Opus como opt-in até A/B maior de latência/CPU antes de promover
  como padrão obrigatório.
- [x] Promover Opus de experimento manual para capability oficial opt-in, com
  status/HELLO/metrics coerentes e fallback PCM16 automático.
- [x] Criar harness pareado PCM16 vs Opus para A/B maior repetível.
- [x] Rodar A/B curto PCM16 vs Opus com zero drops e STT `good`.
- [x] Rodar A/B maior PCM16 vs Opus com zero drops e STT `good`.
- [x] Criar diagnóstico offline de qualidade Opus em cima dos WAVs reais.
- [x] Fixar perfil live inicial em Opus 16 kHz mono, 60 ms, 32 kbps, mantendo
  PCM16 como padrão.
- [x] Repetir A/B live curto no perfil 32 kbps antes de considerar promocao
  local. A promocao local foi aplicada no server com rollback PCM16 preservado.

### Fase 7 — AEC e Modo Realtime

Status: concluida para barge-in por wake word, no-echo e gate de AEC. AEC de
dispositivo fica em standby no hardware atual porque o probe real retornou
`aec_blocked_no_reference=true`, `aec_supported=false` e
`ESP_ERR_NOT_SUPPORTED`. Server-side AEC também fica futuro até existir
referência/timestamps de playback no protocolo.

Objetivo: permitir barge-in e conversa mais natural enquanto o robô fala.

Mudanças:

- Avaliar AEC no firmware apenas depois do AFE estável.
- Usar referência do speaker se o caminho de áudio permitir.
- Ativar `realtime` apenas com AEC validado.
- Server deve distinguir fala do usuário de eco do TTS.
- Antes de qualquer mudança no firmware, medir barge-in real com:
  `noisebot_server debug barge-live "me conte uma historia longa" --json`.
  O comando espera um turno interrompido em `/ai/metrics`, registra
  `discard_reason=barge_in`, `outcome=interrupted` e a latência
  `interruption_cancel_ms`.
- Medir eco/falso follow-up sem AEC com:
  `noisebot_server debug no-echo-live "me conte uma historia longa" --json`.
  O comando espera uma resposta real, abre uma janela de silêncio e falha se
  surgir turno extra em `/ai/metrics` sem fala do usuário.
- O AEC agora tem probe próprio em `/api/audio/processor/aec/probe`: ele cria
  um AFE `MR` de voz (`AFE_TYPE_VC` +
  `AEC_MODE_VOIP_HIGH_PERF`), mede PSRAM/heap interno/DMA e destrói antes de
  retornar. O caminho principal não ativa AEC se a margem de heap estiver
  baixa.
- O comando `noisebot_server debug aec-live --host 192.168.1.30 --json`
  encapsula o probe AEC e classifica se ele é promovível. Resultado seguro
  pode ser `ok=true` com `promotable=false`, quando o firmware permanece vivo
  mas a placa não tem referência limpa do speaker ou margem suficiente.

Validação atual:

- Firmware real, `barge-live` em 2026-05-29:
  - `ok=true`;
  - turno interrompido: `7`;
  - `interruption_cancel_ms=0.9`;
  - `discard_reason=barge_in`;
  - `outcome=interrupted`;
  - transcript original: `Me conte uma história longa.`;
  - reply parcial cancelada: `Havia uma vez, numa vila encantada chamada
    Lumina, um pequeno riu azul que era o lar de milhares de fadas.`
- Firmware real, `no-echo-live` em 2026-05-29:
  - `ok=true`;
  - turno de resposta: `10`;
  - `unexpected_turn_id=null`;
  - janela de silêncio: `10.0s`;
  - `outcome=llm`;
  - transcript: `É muito longa.`;
  - `discard_reason=""`.
- Firmware real, `aec-live` em 2026-05-29:
  - endpoint retornou diagnóstico JSON com HTTP 500, tratado pelo harness sem
    traceback;
  - `aec_supported=false`;
  - `aec_blocked_no_reference=true`;
  - `probe_error=ESP_ERR_NOT_SUPPORTED`;
  - `internal_free_kb=31`;
  - `dma_largest_kb=30`;
  - decisão: `promotable=false`, não promover AEC de dispositivo nesta placa.

Nota de bancada em 2026-05-27: a tentativa de promover WakeNet `MR + AEC`
direto para runtime compilou, mas no hardware causou pressão de memória
observada como `sdmmc_read_sectors: not enough mem`, degradação do SD e queda
de WiFi/bridge. A decisão correta é manter AEC fora do caminho principal até o
probe passar com margem e sem regressão em SD/WiFi.

Critérios de aceite:

- [x] Usuário consegue interromper o robô falando por cima.
- [x] `barge-live` retorna `ok=true` com `discard_reason=barge_in`.
- [x] `interruption_cancel_ms` fica abaixo de 200 ms no teste real.
- [x] `no-echo-live` retorna `ok=true`; o próprio TTS não reabre escuta falsa.
- [x] Sem loops de escuta/resposta no teste live sem eco.
- [x] `aec-live` roda sem derrubar firmware/bridge e retorna recomendação de
  não promoção quando o firmware expõe falta de referência limpa.

### Fase 8 — Produto e Regressão Contínua

Status: iniciada no bridge. A regressão automatizada de protocolo agora cobre o
contrato sem hardware via `bridge/tests/test_fake_firmware.py`, e o contrato
crítico do barge-in no firmware é verificado por
`bridge/tests/test_firmware_audio_service_contract.py`. O fake firmware simula
`HELLO`, `VOICE_START`, `AUDIO_CHUNK`, `VOICE_END`, frames corrompidos, áudio
fora de sessão, sessão vazia seguida de sessão válida, resposta longa em chunks,
STT rejeitado e falha de TTS. O replay agora usa fixtures reais de
`voice_samples/` para cobrir amostras boas e ruins sem hardware, e o CLI aceita
`--replay-dir` para rodar uma pasta inteira e emitir resumo JSON de outcomes.
O baseline versionado fica em `docs/VOICE_REPLAY_BASELINE.json`. A suíte do
bridge está verde com 152 testes. Isso
não substitui o checklist físico do robô, mas impede que a camada de protocolo
volte a aceitar áudio fantasma, responder wake vazio, mascarar falhas de STT/TTS
ou quebrar novamente o contrato mínimo de barge-in.

Objetivo: manter o ciclo funcionando conforme novas features entram.

Mudanças:

- Criar replay de sessões reais boas e ruins.
- Manter fake firmware byte-compatível com o protocolo atual.
- Testar localmente:
  - wake sem fala;
  - fala curta;
  - fala longa;
  - ruído ambiente;
  - TTS interrompido;
  - câmera ativa durante fala;
  - bridge reconectando.
- Colocar esses cenários em checklist antes de release.
- Rodar lote local de amostras:
  `noisebot_bridge --dry-run --replay-dir voice_samples --replay-json`.
- Rodar validação única de release de voz:
  `python bridge/voice_check.py`.
- Para releases locais pos-Opus, aplicar tambem
  `docs/VOICE_AUDIO_V2_RELEASE_CHECKLIST.md`, que exige gates para
  `codec-v2 health`, Playback v2 dono da fila SAY, `capture-v2 status`
  desligado, rollback PCM16, `barge-live`, `no-echo-live` e completude
  TTS/texto em `/ai/metrics`.

Critérios de aceite:

- Nenhuma release sai sem passar o replay básico.
- Fake firmware cobre wake/listen/speak/idle e falhas STT/TTS sem hardware.
- Contrato de barge-in no firmware fica coberto por teste automático antes de
  qualquer novo ajuste de VAD/escuta.
- Checklist de voz roda em comando único e falha se pytest ou baseline de replay
  falharem.
- Toda regressão de voz vira caso de teste antes de mexer em firmware.
- Dashboard dev mostra causa provável antes do usuário precisar ler log.

Pendências:

- Só reabrir follow-up automático ou barge-in por VAD sem wake word depois de
  AEC/AFE validado.
- O corte observado durante a migracao foi investigado sem atribuir ao Opus por
  suposicao. A evidencia mostrou TTS/playback completo e problema visual no
  `TEXT_SCROLL`, corrigido com paginacao server-side por limite UTF-8 e largura
  visual aproximada. Se surgir corte real de audio no futuro, tratar como
  pendencia separada de TTS/streaming, chunking, fila de playback ou criterio
  de fim de fala. Avanco: o server passou a registrar
  diagnostico de completude TTS/playback por turno (`tts_chunks_sent`,
  `tts_pcm_bytes_in`, `tts_pcm_bytes_sent`, `tts_padding_bytes`,
  `tts_say_begin_sent`, `tts_say_end_sent`, `tts_expected_duration_ms`,
  `tts_completed`, `text_scroll_truncated`) para o proximo teste real dizer se
  o corte esta no audio, no `TEXT_SCROLL` visual de 128 bytes ou no envio SAY.
  O `/ai/metrics` agora tambem transforma esses campos em `voice_alert` e
  `voice_diagnosis`: `tts_completed=false` vira alerta de fala possivelmente
  incompleta; truncamento apenas visual do `TEXT_SCROLL` vira diagnostico sem
  alerta.

## Ordem Recomendada

Status de fechamento: a migracao Opus v2 esta concluida como default local do
server, com PCM16 preservado como rollback operacional. O contrato do firmware
continua anunciando Opus como capability opt-in, enquanto o server local liga o
transporte Opus no startup via `NOISEBOT_AUDIO_DEFAULT_CODEC=opus-v2`.

O roadmap detalhado das fases restantes esta em
`docs/VOICE_AUDIO_V2_NEXT_PHASES.md`. A ordem curta agora e:

1. Playback v2 como dono gradual do downlink.
2. Checklist/health de release para proteger Opus, PCM16, texto visual e
   turn-taking. A Fase M parcial esta documentada em
   `docs/VOICE_AUDIO_V2_RELEASE_CHECKLIST.md` e nao altera firmware C.
3. Voice Activity v2 em shadow/opt-in, sem AEC device-side.
   Primeiro incremento local: endpoint firmware `/api/audio/activity-v2` com
   shadow probe passivo alimentado pelo `audio_service`, apenas para telemetria
   RMS/peak/ZCR/fala/silencio/mute/sessao ativa; nao muda wake, captura,
   playback, codec ou bridge. Validado em hardware apos flash com shadow de 1000 ms:
   63 frames observados, silencio classificado, `ESP_OK`, capture-v2 desligado,
   Playback v2 fila zero e `codec-v2 health` ok apos reativar Opus v2. O campo
   ZCR tambem foi validado em hardware com `zcr_last_permille=98` e
   `zcr_max_permille=141` em shadow silencioso de 1000 ms. O status agora
   separa tambem `session_frames`/`idle_frames` e maximos RMS/peak/ZCR de
   frames mutados por playback versus nao mutados, preparando comparacao real
   sem mudar decisao de wake/fim de fala. O incremento local mais recente
   amplia o shadow para ate 30 s e passa contexto explicito de playback do
   `audio_service` para esses buckets, sem alterar VAD, wake, captura, codec,
   Playback v2, bridge ou HAL. Validacao em hardware apos flash confirmou o
   objetivo de observabilidade: em shadow de 30 s durante `ww -> me conte uma
   historia curta`, o endpoint fechou com `session_frames=268`,
   `idle_frames=1607`, `muted_frames=478`, `unmuted_frames=1397`,
   `tts_completed=true`, `SAY_END` e `codec-v2 health` ok. Ponto amarelo:
   Playback v2 contou 14 drops SAY no turno e deve ser repetido antes do
   proximo handoff. Repeticao controlada via `/debug/transcript` apos restart
   correto do server enviou 292 chunks TTS com zero drops novos em Playback v2
   e `codec-v2 health` ok. Repeticoes fisicas por wake mostraram que o caminho
   fisico ainda pode gerar drops na fila SAY (+18 drops em uma resposta curta
   completa), entao o prebuffer padrao do server foi reduzido para 6 chunks
   antes de nova validacao. A validacao fisica seguinte confirmou +326 chunks
   recebidos/tocados e zero drops novos com TTS completo. O incremento local
   seguinte adiciona contadores passivos de sequencias
   consecutivas de fala/silencio (`speech_run_*`, `silence_run_*`) ao shadow
   para futura comparacao de VAD/end-of-speech, ainda sem mudar wake, captura,
   playback, codec ou bridge. Validacao local: contrato focado Voice Audio v2
   e build ESP-IDF limpos; em hardware apos flash, shadow de 1000 ms registrou
   63 frames de silencio com `silence_run_max_frames=63`,
   `speech_run_max_frames=0`, Playback v2 fila zero, Capture v2 desligado e
   `codec-v2 health` ok. A validacao real seguinte rodou shadow de 30 s
   durante `ww -> me conte uma historia curta`: Activity v2 registrou
   `session_frames=384`, `muted_frames=334`, `speech_frames=45`,
   `speech_run_max_frames=7` e `silence_run_max_frames=521`, enquanto o turno
   teve transcript bom, TTS completo, zero drops novos em Playback v2, Capture
   v2 desligado e Codec v2 ok. Nota operacional: se o server subir sem
   `NOISEBOT_HOST` ou `--host`, ele fica
   vivo mas sem transporte (`connected=false`), o que parece queda de voz. O
   `.env` local deve conter `NOISEBOT_HOST=192.168.1.30` junto de
   `NOISEBOT_AUDIO_DEFAULT_CODEC=opus-v2`.
4. Capture Session v2 assumindo upstream por flag.
5. Policy conversacional avancada somente depois de no-echo/captura estaveis.

Historico de fechamento Opus:

1. Preservar wake/VAD/turn-taking atual: sem ajuste novo sem regressão
   comprovada e teste.
2. Promover Opus para capability oficial opt-in com fallback PCM16. Feito no
   contrato local: `codecs` descreve o transporte ativo (`pcm16=true`,
   `opus=false` por padrao) e `codec_options` anuncia suporte opt-in a
   `opus_tx` com `opus_default=false`, 16 kHz mono, frames de 60 ms/960 samples
   e 32 kbps. `BRIDGE_HELLO_V2_OPUS` continua reservado para transporte Opus
   ativo apos enable explicito.
3. Validar em hardware o HELLO/status novo apos flash e confirmar que
   `/api/ai/status` espelha `codec_options`. Feito apos flash: `/ai/status`
   confirmou PCM16 default, `codec_options.opus_tx=true` e
   `opus_default=false`; enable/disable de Opus alternou o transporte ativo e
   rollback voltou para PCM16 com worker parado, fila zero e `capture-v2`
   desligado.
4. Ampliar regressão automática de protocolo, incluindo reconexão e
   cancelamento explícito.
5. Só depois avaliar se Opus deve virar padrão obrigatório.
   Etapa intermediaria feita: o server aceita
   `NOISEBOT_AUDIO_DEFAULT_CODEC=opus-v2` ou `--audio-codec opus-v2` para subir
   ja habilitando `codec-v2 transport-enable`, mantendo `pcm16` como default de
   fabrica e rollback simples. Validacao live confirmou startup em Opus e volta
   limpa para PCM16. Soak real com intents locais e LLM em Opus v2 confirmou
   579 pacotes processados/drenados, zero drops, fila zero e codec sem erro.
   Promocao local aplicada em `server/.env` com
   `NOISEBOT_AUDIO_DEFAULT_CODEC=opus-v2`; reinicio sem flag confirmou Opus
   ativo, 738 pacotes processados/drenados, zero drops, fila zero e
   `capture-v2` desligado.
6. AEC/realtime/follow-up continuam standby até existir referência limpa de
   playback ou server-side AEC validado.
7. Usar os novos campos de completude TTS/playback em `/ai/metrics` para
   investigar o corte de texto/voz depois de confirmar a migração Opus em uso
   diário. O endpoint ja diferencia `tts_completed=false` de
   `text_scroll_truncated=true`. A validacao real com resposta longa mostrou
   TTS/playback completo (`SAY_END`, 589 chunks, ~9,4 s esperados) e apenas
   truncamento visual; `/ai/metrics` preserva `reply` longo para diagnostico.
   Avanco: o server pagina respostas longas em multiplos `TEXT_SCROLL` UTF-8
   seguros de ate 128 bytes, sem novo opcode e sem mudanca no firmware. As
   metricas expõem `text_scroll_pages` e `text_scroll_pages_sent`. Refino:
   paginas tambem sao limitadas por largura visual aproximada de 38 caracteres,
   porque frases medias podem caber em 128 bytes e ainda assim depender do
   scroll horizontal lento do overlay. Esse ajuste e server-only, preserva o
   protocolo e foi validado com `server/tests` verde.
8. Manter um guardrail operacional para Opus v2 ativo. O server agora expoe
   `codec-v2 health` no CLI e `/api/device/audio/codec-v2/health` no proxy,
   classificando o status do firmware em `ok`, `warn` ou `degraded` com base
   em drops, erro do codec, worker, fila pronta e fila egress. Validacao local:
   `server/tests` com 154 testes verdes. Validacao live: uma fila egress
   pendente de 1 pacote foi detectada como warning, drenada com
   `codec-v2 egress-drain`, e o health voltou limpo com zero drops e
   `opus_codec_error=0`.
9. Fechamento concluido: `codec-v2 health` retornou `healthy=true`,
   `status=ok`, sem issues/warnings, zero drops, fila egress zero,
   `opus_codec_error=0` e worker `running`. `/ai/status` confirmou server
   conectado em `qwen3.5:9b` e audio ativo em Opus 16 kHz mono, 60 ms.
   Rollback segue por `NOISEBOT_AUDIO_DEFAULT_CODEC=pcm16` + restart ou
   `codec-v2 transport-disable`.

Essa ordem evita a armadilha de trocar codec, VAD, AEC e STT ao mesmo tempo. O
fim desejado é ambicioso, mas cada fase precisa ter medição própria para o robô
continuar utilizável todos os dias.
