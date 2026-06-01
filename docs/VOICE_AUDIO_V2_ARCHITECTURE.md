# Voice Audio v2 Architecture

Data: 2026-05-30
Status: arquitetura vigente do refactor paralelo, sem substituir o rollback PCM16.
Branch de trabalho: `voice-reference-architecture`.

Este documento fixa o mapa tecnico para refazer o subsistema de voz do NoiseBot
sem perder o que ja funciona no hardware atual.

Versao para consulta rapida em Obsidian/IA:
`docs/OBSIDIAN_VOICE_AUDIO_V2_KNOWLEDGE.md`.

Roadmap operacional das fases restantes apos o fechamento do Opus:
`docs/VOICE_AUDIO_V2_NEXT_PHASES.md`.

Checklist/health de release da Fase M parcial:
`docs/VOICE_AUDIO_V2_RELEASE_CHECKLIST.md`.

O objetivo nao e "reescrever tudo". O objetivo e retirar do `audio_service.c`
as responsabilidades que foram acumuladas ao longo da migracao e criar um
pipeline v2 paralelo, testavel, atras de flag, com rollback imediato para o
pipeline PCM16 atual.

## Regras de Protecao

- PCM16 atual permanece o fallback seguro ate o v2 passar em testes e hardware.
- Wake word atual nao deve ser refeito nesta etapa.
- Barge-in por wake word atual nao deve ser refeito nesta etapa.
- State machine, expressividade, intents locais, STT/LLM/TTS do server e bridge
  atual nao devem ser reescritos junto com audio v2.
- AEC so entra quando houver referencia real de playback ou decisao explicita de
  AEC server-side. Nao promover AEC device-side sem canal de referencia.
- Cada fase deve gerar commit proprio, build limpo e criterio de rollback.

## Premissas Tecnicas

- Fluxo separado:
  - mic -> processor -> encode queue -> Opus encoder -> send queue -> server.
  - server -> decode queue -> Opus decoder -> playback queue -> speaker.
- Um caminho de I/O de audio separado do codec:
  - task de input;
  - task de output;
  - task dedicada para Opus encode/decode.
- Opus:
  - 16 kHz mono;
  - frame de 60 ms;
  - `ESP_OPUS_ENC_APPLICATION_AUDIO`;
  - complexidade 0;
  - FEC desligado;
  - DTX ligado;
  - VBR ligado;
  - filas curtas em torno de 2400 ms / 60 ms = 40 pacotes.
- Protocolo:
  - `hello.audio_params.format = "opus"`;
  - `sample_rate = 16000`;
  - `channels = 1`;
  - `frame_duration = 60`;
  - downlink pode ter sample rate diferente e ser resamplado.
- Estados de conversa:
  - Idle;
  - Connecting;
  - Listening;
  - Speaking;
  - abort/close volta ao Idle.
- Wake durante fala:
  - a aplicacao envia abort/cancel;
  - interrompe speaking;
  - reabre listening.
- AFE:
  - `AFE_TYPE_VC`;
  - `AFE_MODE_HIGH_PERF`;
  - `AEC_MODE_VOIP_HIGH_PERF`;
  - VAD mode 0;
  - `vad_min_noise_ms = 100`;
  - NSNET quando modelo existe;
  - AGC desligado;
  - alocacao preferencial em PSRAM.

## Divergencias do NoiseBot

Hardware atual:

- ESP32-S3 N16R8.
- INMP441 como microfone digital I2S.
- MAX98357A como speaker I2S.
- Sem codec externo dedicado para microfone.
- Sem codec externo dedicado para speaker.
- Sem canal limpo de referencia de speaker no input.
- I2S atual em 16 kHz, mono logico, com fio 32-bit/stereo no HAL.

Consequencias:

- A arquitetura pode separar I/O, playback, captura, codec e policy.
- Nao podemos promover AEC device-side sem referencia limpa de playback.
- AFE sem referencia pode ser usado para VAD/NS, mas nao deve ser vendido como
  AEC real.
- Server-side AEC so faz sentido se adicionarmos timestamps de playback e
  referencia/logica no server.
- O caminho PCM16 precisa continuar existindo porque e mais simples, mais
  observavel e ja esta validado.

## Estado Atual do NoiseBot

Arquivos centrais:

- `components/nb_hal/audio_hal.c`
- `components/nb_hal/audio_hal.h`
- `components/services/audio_service/audio_service.c`
- `components/services/audio_service/audio_service.h`
- `components/services/audio_processor_service/audio_processor_service.c`
- `components/services/audio_processor_service/audio_processor_service.h`
- `components/infra/bridge_service.c`
- `components/infra/bridge_service.h`
- `components/behavior/voice_controller/voice_controller.c`
- `components/services/wake_service/wake_service.c`
- `components/services/sound_analysis_service/sound_analysis_service.c`
- `components/services/vad_semantic_service/vad_semantic_service.c`
- `components/services/synth_service/synth_service.c`

O que esta bom e deve ser preservado:

- PCM16 como fallback de voz.
- Wake word local.
- Barge-in por wake word durante resposta.
- `VOICE_START`, `AUDIO_CHUNK`, `VOICE_END`.
- Bridge protocol v2.
- Server decode de Opus para PCM antes do STT.
- Harnesses: `codec-ab`, `opus-live`, `barge-live`, `no-echo-live`,
  `opus-quality` e fake firmware.
- Intents locais, incluindo fallback pt-BR de curiosidade.
- Politica de nao responder conversa ambiente sem wake valido.

O que esta acumulado demais hoje:

- `audio_service.c` concentra playback local, playback SAY, silencio TX, mic,
  high-pass, sound analysis, ESP-SR VAD, heuristica RMS/ZCR/espectral,
  pre-roll, wake feed, AFE shadow, sessao de escuta, bridge TX, Opus
  feed/drain, diagnostico WAV e recuperacao I2S.
- `audio_processor_service.c` concentra AFE probe, AEC probe, shadow processor,
  fonte processada para bridge, Opus worker e fila de pacotes Opus.

Esse acumulo e a principal causa de regressao: uma mudanca pequena em VAD, Opus
ou playback passa pelo mesmo loop de 16 ms e pode afetar wake, barge-in, STT ou
SAY.

## Arquitetura Alvo

```text
Layer 1 HAL
  audio_hal
    - I2S RX/TX bruto
    - sem VAD, sem bridge, sem codec

Layer 4 Services
  audio_io_service_v2
    - task(s) de input/output
    - PCM16 mono normalizado
    - silencio TX
    - recuperacao I2S

  audio_playback_service_v2
    - WAV/PCM local
    - SAY do bridge
    - fila curta
    - stop/cancel
    - volume digital

  voice_activity_service_v2
    - ESP-SR VAD principal
    - heuristica apenas diagnostica
    - AFE/NS opcional
    - eventos speech_start/speech_end internos

  voice_capture_session_v2
    - wake/barge/follow-up abre sessao
    - pre-roll
    - timeouts
    - VOICE_START/VOICE_END
    - entrega frames ao codec/transporte

  audio_codec_service_v2
    - PCM16 passthrough
    - Opus 16 kHz mono, 60 ms, 32 kbps opt-in
    - filas curtas
    - worker dedicado

Layer 2 Infra
  bridge_service
    - transporte
    - HELLO/capabilities
    - envio AUDIO_CHUNK
    - recepcao SAY/SESSION events

Layer 6 Behavior
  voice_controller
    - politica: wake, barge-in, follow-up
    - nao faz DSP
```

## Contratos Internos Propostos

### PCM Frame

Unidade padrao interna:

- sample rate: 16000 Hz;
- canais: 1;
- amostra: `int16_t`;
- chunk base HAL: 256 samples / 16 ms;
- frame Opus: 960 samples / 60 ms.

Estrutura conceitual:

```c
typedef struct {
    const int16_t *samples;
    uint16_t sample_count;
    uint32_t timestamp_ms;
    uint8_t source_flags;
} nb_audio_pcm_frame_t;
```

### Audio I/O v2

Responsabilidade:

- Ler mic.
- Escrever speaker ou silencio.
- Publicar PCM capturado para consumidores internos.
- Receber PCM de playback.
- Nao decidir quando uma conversa comeca.
- Nao enviar bridge.
- Nao chamar LLM/STT.

Invariantes:

- Sem malloc no loop de I/O.
- Buffers estaticos ou PSRAM prealocada.
- DMA em SRAM.
- TX nunca fica sem alimentacao.
- Recuperacao I2S isolada e mensuravel.

### Playback v2

Responsabilidade:

- Tocar WAV/PCM local.
- Tocar SAY do bridge.
- Aplicar volume.
- Cancelar imediatamente quando `audio_play_stop()` ou barge-in pedir.

Invariantes:

- Fila SAY deve ser drenada/descartada no cancel.
- Nenhum chunk antigo pode tocar depois de `SPEECH_CANCEL`.
- Playback nao deve alimentar VAD como fala do usuario.
- Deve haver janela de mute/ignore para VAD pos-playback.

### Voice Activity v2

Responsabilidade:

- No incremento inicial da Fase J, observar passivamente o PCM condicionado
  que ja passa pelo `audio_service`, expondo `/api/audio/activity-v2` e
  shadow start/stop para medir RMS/peak, fala/silencio, frames mutados e
  sessao ativa sem alterar wake, captura, bridge, codec ou playback.
- O shadow tambem expoe ZCR em `zcr_last_permille` e `zcr_max_permille`,
  calculado sem float/no malloc, como telemetria comparativa. ZCR ainda nao
  participa de threshold nem de decisao de wake/fim de fala.
- O shadow separa a telemetria por contexto: `session_frames` vs `idle_frames`
  e maximos RMS/peak/ZCR para frames `muted` e `unmuted`. Esses campos existem
  para comparar fala real, silencio e vazamento do playback durante testes
  `ww -> resposta -> ww`; nao alteram wake, captura, bridge, codec ou playback.
- A janela maxima do shadow agora e 30 s para cobrir um ciclo fisico completo
  `ww -> STT -> LLM -> TTS -> barge-in/idle`. O contexto de playback enviado
  ao Activity v2 e explicito (`wrote_audio`, estado de playback e fila SAY v2),
  mas continua sendo usado somente para bucket de telemetria; VAD, wake,
  captura, codec, Playback v2 e HAL permanecem nos caminhos ja validados.
- Validacao em hardware do shadow 30 s confirmou o objetivo de observabilidade:
  turno real `ww -> me conte uma historia curta` gerou
  `session_frames=268`, `idle_frames=1607`, `muted_frames=478`,
  `unmuted_frames=1397`, com Opus v2 saudavel e Capture v2 desligado. O mesmo
  turno registrou 14 drops SAY em Playback v2, entao playback deve ser
  rechecado antes de novo handoff. Repeticao controlada via `/debug/transcript`
  depois de restart correto do server confirmou o caminho do orquestrador sem
  drops novos: +292 chunks SAY recebidos/tocados e `say_chunks_dropped`
  inalterado. Repeticoes fisicas por wake mostraram que o caso real ainda
  precisa de headroom: uma rodada teve +222 chunks sem drops, mas transcript
  diferente do comando esperado; a rodada seguinte ouviu
  `Me fala em historia curta.`, completou TTS e `SAY_END`, porem somou
  +18 drops enquanto tocava +274 chunks. O ajuste atual e server-only:
  reduzir o prebuffer default do `OutputScheduler` para 6 chunks
  (`NOISEBOT_TTS_QUEUE_TARGET`), sem alterar firmware, wake, captura, codec ou
  HAL. Validacao fisica seguinte confirmou o ajuste: `Me diga uma fala com
  história curta.` gerou 326 chunks TTS completos e Playback v2 recebeu/tocou
  +326 chunks com `say_chunks_dropped` inalterado em 56.
- Validacao em hardware pos-flash confirmou o contrato passivo: shadow de
  1000 ms observou 63 frames, encerrou sozinho, classificou silencio e manteve
  `session_active=false`; `capture-v2` permaneceu desligado, Playback v2 ficou
  com fila SAY zero e `codec-v2 health` voltou ok apos reativar Opus v2.
- Classificar fala/silencio dentro de sessao ja aberta.
- Usar ESP-SR VAD como caminho principal.
- Manter RMS/ZCR/espectral como telemetria e fallback de bancada.

Invariantes:

- VAD nao abre sessao sozinho em IDLE.
- VAD so pode gerar `speech_start` publico quando ha sessao ativa.
- Thresholds nao podem ser ajustados para corrigir wake word sem evidencia.
- Logs devem diferenciar wake aceito/rejeitado, sessao aberta, fala detectada,
  audio enviado e audio descartado.

### Capture Session v2

Responsabilidade:

- Controlar ciclo `IDLE_SESSION -> WAITING_FOR_SPEECH -> CAPTURING ->
  ENDING_ON_SILENCE -> DONE`.
- Gerenciar pre-roll.
- Enviar `VOICE_START`.
- Enviar audio.
- Enviar `VOICE_END`.
- Durante o inicio da Fase K, expor no status o motivo de fim
  (`end_reason`) e o ownership real do TX (`bridge_tx_owner=false`,
  `legacy_audio_service_tx_owner=true`) antes de assumir qualquer envio real ao
  bridge. Esse primeiro passo e observabilidade/contrato, nao handoff ativo.
- O passo seguinte da Fase K adiciona espelhamento shadow de TX
  (`shadow_voice_start_sent`, `shadow_voice_end_sent`, `shadow_audio_chunks`,
  `shadow_audio_samples`, `shadow_audio_dropped_chunks`) para comparar o que o
  Capture v2 emitiria contra o caminho legado. O bridge TX real continua no
  `audio_service`.
- O gate local de handoff tambem e apenas observabilidade: `bridge_tx_candidate`,
  `bridge_tx_handoff_ready` e `handoff_block_reason` dizem se uma sessao real
  observada pelo Capture v2 ja cumpre as condicoes minimas para virar candidata
  a ownership de bridge TX. Nenhum `VOICE_START`, `AUDIO_CHUNK` ou `VOICE_END`
  passa a ser enviado pelo Capture v2 neste passo.

Parametros iniciais preservados:

- espera por inicio de fala: 8000 ms;
- silencio de fim: 900 ms;
- teto de fala: 9200 ms;
- pre-roll: 20 chunks de 256 samples = 320 ms;
- barge-in: pre-roll suprimido se ele trouxer audio velho do TTS.

Invariantes:

- Wake vazio nao envia `VOICE_START`.
- Sessao sem audio nao envia `VOICE_END` para STT.
- `VOICE_END` so ocorre se houve `VOICE_START` e audio.
- Cancelamento limpa fila TX antes de encerrar.
- Follow-up automatico permanece desabilitado ate validacao especifica.

### Codec v2

Responsabilidade:

- Receber PCM16 da sessao.
- Se codec = PCM16, enviar chunks PCM16.
- Se codec = Opus, acumular 960 samples, codificar e enviar pacote.

Perfil Opus inicial:

- 16 kHz;
- mono;
- 60 ms;
- 32 kbps;
- application audio;
- complexity 0;
- FEC off;
- DTX on;
- VBR on;
- fila curta, alvo maximo 40 pacotes.

Justificativa:

- 60 ms e filas curtas foram validados no contexto do NoiseBot como bom
  equilibrio entre latencia, overhead e previsibilidade de fila.
- 32 kbps foi escolhido por diagnostico offline em WAVs reais do NoiseBot:
  melhor SNR/correlacao do que 16/24 kbps.
- PCM16 continua padrao porque o A/B live ainda mostrou variacao semantica.

### Bridge

Responsabilidade preservada:

- Transportar mensagens.
- Manter HELLO/capabilities.
- Expor PCM16/Opus ao server.
- Receber SAY e session events.

Nao deve:

- Fazer VAD.
- Decidir wake.
- Ajustar ganho de mic.
- Corrigir janela de captura.

## Mapeamento Arquitetural do NoiseBot

| Tema | NoiseBot v2 |
| --- | --- |
| Codec upstream | Opus opt-in 16 kHz mono, 60 ms, 32 kbps |
| Fallback | PCM16 mantido como rollback operacional |
| Input task | `audio_io_service_v2` como caminho futuro de entrada |
| Output task | `audio_playback_service_v2` como caminho gradual de downlink |
| Codec task | `audio_codec_service_v2` com worker dedicado e stack em PSRAM |
| Processor | `voice_activity_service_v2` + AFE/VAD/NS opcional |
| Wake | Preservar `wake_service` atual |
| Barge-in | Preservar caminho por wake word |
| AEC device | Nao promover sem referencia limpa de playback |
| AEC server | Futuro, se houver timestamps e referencia de playback |
| Downlink | SAY PCM atual, com handoff gradual para playback v2 |
| Estado | Mapear para IDLE/ATTENTIVE/RESPONDING sem reescrever state machine |

## O Que Nao Entra Nesta Fase

- Codec externo dedicado: o NoiseBot atual usa INMP441/MAX98357A.
- `AUDIO_INPUT_REFERENCE=true`: nao existe canal limpo de referencia no
  hardware atual.
- AEC device-side: sem referencia, vira consumo de memoria sem garantia.
- Input 24 kHz como requisito: o caminho atual e 16 kHz; STT e Opus upstream ja
  estao em 16 kHz.
- Troca completa de transporte: o bridge TCP local ja funciona e possui testes.
- C++/STL fora do display: o firmware NoiseBot e C17; usar filas FreeRTOS e
  buffers estaticos/PSRAM.

## Riscos e Contramedidas

| Risco | Causa Provavel | Contramedida |
| --- | --- | --- |
| Wake parar de ouvir | Captura v2 disputa mic ou muda feed | manter wake_service no caminho atual ate fase explicita |
| Robo responder ambiente | VAD abre sessao em IDLE | sessao so abre por wake/barge/follow-up |
| Barge-in cortar mas nao escutar | cancel limpa TTS mas sessao nao abre captura | teste `barge-live` obrigatorio em cada troca de sessao |
| Audio fantasma pos-TTS | playback realimenta mic/VAD | mute window + `no-echo-live` |
| STT receber audio curto/vazio | `VOICE_START/END` sem fala real | sessao so envia END se houve audio |
| Opus piorar semantica | bitrate/janela/volume/captura | A/B `codec-ab`, PCM fallback, Opus opt-in |
| Fila atrasar fala | backpressure de bridge/codec | filas curtas, contador de drops, abort limpo |
| I2S crash/recover ruim | TX/RX juntos e recover no loop | I/O v2 com recuperacao isolada e telemetria |
| AEC consumir memoria | AFE com config inadequada | AEC bloqueado sem reference; probe nao promove |
| Regressao por mudanca grande | ativar tudo de uma vez | flags por fase, commit por fase, build/teste por fase |

## Plano de Migracao

### Fase A - Documento e Contrato

Objetivo: congelar mapa tecnico e evitar refactor improvisado.

Entregas:

- este documento;
- link em `VOICE_PIPELINE.md`;
- link em `ROADMAP.md`;
- nenhum comportamento alterado.

Aceite:

- Sem alteracao em firmware funcional.
- Commit proprio.

### Fase B - Esqueleto v2 Inativo

Status: concluida em `e7dfea2` (`Firmware: iniciar esqueleto de audio v2`).

Objetivo: criar componentes vazios/contratos sem ativar.

Entregas:

- `components/services/audio_io_service_v2`;
- `components/services/audio_playback_service_v2`;
- `components/services/voice_activity_service_v2`;
- `components/services/voice_capture_session_v2`;
- `components/services/audio_codec_service_v2`.

Aceite:

- [x] Compila com `-Wall -Wextra -Werror`.
- [x] Nenhum componente v2 inicializado no boot.
- [x] Teste de contrato confirma que v1 ainda e o caminho ativo.

Validacao:

- `idf.py build` concluido sem warnings.
- `bridge/tests/test_firmware_audio_v2_skeleton_contract.py` confirma que os
  componentes existem e que o `boot_manager` nao inclui nem inicializa v2.
- `bridge/tests` passou com 156 testes.

### Fase C - Audio I/O v2 em Probe

Status: iniciada em `563fd3c` (`Firmware: adicionar probe passivo de audio io v2`).

Objetivo: validar leitura/escrita sem conversa.

Entregas:

- `GET /api/audio/io-v2` com status do probe;
- `POST /api/audio/io-v2/probe` para armar janela curta de probe;
- `POST /api/audio/io-v2/probe/stop` para encerrar manualmente;
- metricas de chunks RX, TX silencio, drops, RMS/peak e heap.

Implementacao atual:

- O probe e passivo: `audio_service` continua dono unico do `audio_hal`.
- O v2 recebe copia do PCM16 ja condicionado (`s_sa_buf`) somente enquanto o
  probe esta ativo.
- O v2 contabiliza TX de silencio quando o loop v1 ja esta alimentando o
  speaker com silencio.
- Nao ha task v2, disputa de I2S, bridge, wake, VAD, Opus ou playback v2.

Aceite:

- [x] Nao toca bridge.
- [x] Nao toca wake.
- [x] Nao altera playback atual.
- [x] Build limpo.

Validacao:

- `idf.py build` concluido sem warnings.
- `bridge/tests/test_firmware_audio_v2_skeleton_contract.py` garante endpoint
  explicito e feed passivo.
- `bridge/tests` passou com 157 testes.
- Firmware real via OTA em 2026-05-30:
  - `POST /api/audio/io-v2/probe` com `duration_ms=1000`;
  - resultado: `rx_frames=63`, `tx_silence_frames=63`,
    `probe_elapsed_ms=1008`, `dropped_frames=0`, `i2s_recoveries=0`;
  - audio/heap pos-probe saudavel: `health=100`, SD montado,
    `heap_internal_free=31655`, `heap_dma_free=31651`.

### Fase D - Playback v2 em Probe

Status: iniciada em `99a3a17` (`Firmware: adicionar probe de playback v2`).

Objetivo: tocar WAV/SAY sintetico pelo novo caminho sem usar conversa real.

Entregas:

- `GET /api/audio/playback-v2` com status do probe;
- `POST /api/audio/playback-v2/probe` para tocar chunk sintetico curto;
- `POST /api/audio/playback-v2/stop` para cancelamento explicito;
- stop/cancel v2 limpa chunks pendentes;
- teste de contrato garante que playback v2 nao chama `audio_hal`.

Implementacao atual:

- `audio_playback_service_v2` gera tom sintetico PCM16 de bancada.
- O servico v2 nao cria task e nao possui o I2S.
- O `audio_service` atual continua sendo o unico escritor do speaker e puxa o
  chunk v2 somente quando o probe explicito esta ativo.
- `audio_play_stop()` tambem cancela o probe v2 se ele estiver tocando.
- O endpoint de probe recusa iniciar se `audio_service_is_busy()` indicar
  escuta, WAV, SAY ou outro playback ativo.
- Pos-Opus, o primeiro incremento da Fase I fez o playback v2 observar o
  downlink SAY real: `audio_service` notifica enqueue/play/drop/cancel/idle,
  e `/api/audio/playback-v2` passou a expor contadores de fila SAY. Isso ainda
  nao troca a fila, nao toca HAL e nao altera o caminho de audio.

Aceite:

- [x] Sem audio velho apos cancel.
- [x] Sem crash por fila cheia.
- [x] Pipeline v1 segue ativo por padrao.

Validacao:

- `idf.py build` concluido sem warnings.
- `bridge/tests/test_firmware_audio_v2_skeleton_contract.py` garante endpoint
  explicito, escrita via `audio_service` e ausencia de chamada HAL no v2.
- `bridge/tests` passou com 160 testes.
- Firmware real via OTA em 2026-05-30:
  - probe curto: `duration_ms=320`, `amplitude=1200`,
    `played_chunks=20`, `queued_chunks=0`, `dropped_chunks=0`;
  - cancelamento: probe de `1000 ms` interrompido por
    `POST /api/audio/playback-v2/stop`, resultado `playing=false`,
    `queued_chunks=0`, `cancel_count=1`, `dropped_chunks=0`;
  - pos-probe: robo voltou a `IDLE`, `health=100`.
- Incremento pos-Opus de observabilidade SAY:
  - campos novos no status: `bridge_say_observer`, `say_queue_depth`,
    `say_queue_count`, `say_chunks_received`, `say_chunks_played`,
    `say_chunks_dropped`, `say_chunks_dropped_listening`,
    `say_chunks_cancelled` e `say_cancel_count`;
  - validacao local: contrato bridge focado, server facade, `bridge/tests` e
    build ESP-IDF;
  - validacao em hardware pendente apos flash.

### Fase E - Capture Session v2 com PCM16

Status: captura PCM16 real acompanhada por v2 atras de flag opt-in.

Objetivo: uma sessao real por wake usando PCM16, atras de flag.

Entregas:

- abrir sessao por wake;
- pre-roll;
- VAD start/end;
- `VOICE_START/AUDIO_CHUNK/VOICE_END` PCM16.

Implementacao:

- `voice_capture_session_v2` agora possui maquina de estado exercitavel por
  replay sintetico.
- Endpoints HTTP explicitos:
  - `GET /api/audio/capture-v2`;
  - `POST /api/audio/capture-v2/replay`;
  - `POST /api/audio/capture-v2/cancel`.
- O replay simula fala/silencio e contabiliza `voice_start_sent`,
  `voice_audio_sent`, `voice_end_sent`, frames, amostras e source.
- Replay silencio-only termina sem `voice_start_sent` e sem
  `voice_audio_sent`, cobrindo a regra "wake vazio nao envia STT".
- O componente nao chama `bridge_service`, nao publica `VOICE_START`,
  `AUDIO_CHUNK` ou `VOICE_END`, e nao e inicializado no boot.
- Flag opt-in persistente:
  `voice_audio_v2_capture_enabled=false` por padrao (`v2cap_en=0` em NVS).
- O schema NVS nao foi incrementado para evitar reaplicar defaults existentes;
  a chave nova e migrada pontualmente com valor `0` quando ausente.
- `audio_service_begin_listen_session()` possui hook condicional: se a flag
  estiver desligada, segue v1; se ligada, tenta `voice_capture_session_v2`.
- A captura PCM16 real v2 agora inicia estado de sessao atras da flag, mas o
  envio de PCM16 segue passando pelo `audio_service`, que continua dono do
  bridge/audio ja validado.
- `voice_capture_session_v2` acompanha `VOICE_START`, chunks aceitos/drops e
  fim/cancelamento da sessao sem chamar `bridge_service` diretamente.
- O status HTTP inclui `real_capture` para diferenciar replay sintetico de
  sessao PCM16 real, facilitando validacao em hardware sem mudar o caminho v1.
- `POST /api/audio/capture-v2/cancel` encerra a sessao real via
  `audio_service_end_listen_session(NB_LISTEN_END_CANCELLED)` quando a captura
  v2 e o audio_service estao ativos, mantendo os estados sincronizados.
- O server possui cliente/proxy diagnostico para captura v2:
  `/api/device/audio/capture-v2`,
  `/api/device/audio/capture-v2/replay`,
  `/api/device/audio/capture-v2/cancel`, alem do comando
  `noisebot_server debug capture-v2` para status/replay/cancel e validacao
  `live` com rollback automatico da flag.

Aceite:

- `codec-ab` PCM16 ok.
- `barge-live` ok.
- `no-echo-live` ok.
- TV/ambiente sem wake nao abre sessao.

Validacao:

- `idf.py build` concluido sem warnings.
- `bridge/tests/test_firmware_audio_v2_skeleton_contract.py` garante endpoint
  explicito, ausencia de chamadas ao bridge no capture v2 e cancelamento da
  sessao real pelo `audio_service`.
- `bridge/tests` passou com 160 testes.
- Flag e hook opt-in criados em commits pequenos:
  - `Firmware: adicionar flag de captura v2`;
  - `Firmware: rotear captura v2 como opt-in`.
- Firmware real via `noisebot_server debug capture-v2 live --json`:
  - antes: `real_capture_enabled=false`, `initialized=false`;
  - depois do turno real: `real_capture_enabled=true`,
    `real_capture=true`, `state=DONE`, `source=WAKE_WORD`;
  - `voice_start_sent=true`, `voice_audio_sent=true`,
    `voice_end_sent=true`;
  - `speech_frames=260`, `captured_samples=66560`,
    `dropped_frames=0`, `last_error=ESP_OK`;
  - rollback: `disabled.ok=true`.
- Regressao `barge-live` com flag v2 desligada:
  - primeira execucao mostrou sucesso operacional nas metricas, mas falso
    negativo do harness; o harness foi ajustado para aceitar contadores
    agregados `interruption_cancel`/`turns.interrupted`;
  - segunda execucao: `ok=true`, `interrupted_turn_id=85`,
    `interruption_cancel_ms=1.4`, `discard_reason=barge_in`,
    `outcome=interrupted`.
- Regressao `no-echo-live` com flag v2 desligada:
  - `ok=true`, `response_turn_id=87`, `unexpected_turn_id=null`,
    `quiet_window_s=10.0`, `outcome=llm`, sem reabertura de escuta por eco.
- Turno normal com flag v2 desligada:
  - firmware status antes/depois: `real_capture_enabled=false`,
    `session_active=false`;
  - servidor: novo `turn_id=88`, `outcome=local_intent`,
    `intent_name=local_time`, `transcript_quality=good`,
    `discard_reason=null`, `error_stage=null`;
  - transcript: `Que horas são?`; resposta: horario local retornado.
- Proxima etapa: decidir promocao controlada da proxima fase mantendo a captura
  v2 atras da flag e v1 como padrao.

### Fase F - Codec v2 Opus

Status: iniciada como observabilidade/contrato, sem ativar transporte v2.

Objetivo: plugar Opus novo no v2 sem mexer em VAD.

Entregas:

- status HTTP explicito em `GET /api/audio/codec-v2`;
- contrato publicado: PCM16 default, Opus opt-in, 16 kHz mono, 60 ms,
  960 samples, 32 kbps e fila curta de ate 40 pacotes;
- worker dedicado opt-in para drenar a fila sintetica;
- fila curta limitada e observavel;
- HELLO/capabilities coerentes.

Implementacao atual:

- `audio_codec_service_v2` continua inativo e nao e inicializado no boot;
- `GET /api/audio/codec-v2` retorna constantes do contrato e contadores
  zerados do skeleton;
- o status do codec v2 expoe o worker opt-in:
  `worker_supported=true`, `worker_active=false`,
  `worker_state=not_started`, `worker_drained_packets`,
  `worker_opus_packets`, `worker_opus_encoded_bytes_total` e
  `worker_opus_last_packet_bytes`; o boot nao cria task;
- `POST /api/audio/codec-v2/encode-test` executa um teste sintetico PCM16
  passthrough: incrementa `pcm_frames_in` e `packets_out`, mantendo
  `packet_drops=0` e sem fila pendente;
- o packetizer sintetico acumula chunks PCM16 de 256 samples ate formar um
  frame de 960 samples; o `encode-test` alimenta 4 chunks, gerando 1 pacote e
  deixando `pending_samples=64`;
- a fila sintética do codec v2 aceita até 40 pacotes; pacotes completos acima
  desse limite incrementam `packet_drops`, sem alocar memoria e sem enviar ao
  bridge;
- `POST /api/audio/codec-v2/drain` drena a fila sintetica pronta e retorna
  `drained_packets`, preservando `pending_samples` e os contadores acumulados;
- `POST /api/audio/codec-v2/reset` zera contadores, fila e amostras pendentes,
  preservando o contrato fixo e `format=pcm16`;
- `POST /api/audio/codec-v2/opus-encode-test` executa o primeiro encode Opus
  real dentro do `audio_codec_service_v2`: cria uma task temporaria com stack
  proprio, abre o encoder Espressif, codifica um frame sintetico de 960
  samples, fecha o encoder e retorna bytes/heap/erro; nao cria worker
  persistente, nao envia ao bridge, nao altera captura/playback e nao muda o
  formato padrao PCM16;
- `POST /api/audio/codec-v2/overflow-test` executa teste diagnostico
  autocontido: limpa estado no inicio, tenta enfileirar N pacotes completos,
  reporta aceitos/drops/pico de fila e limpa estado ao final;
- `POST /api/audio/codec-v2/worker/start` cria a task FreeRTOS
  `nb_codec_v2_worker` apenas sob comando explicito; ela abre o encoder Opus
  no contexto do worker, consome a fila sintetica, codifica um frame sintetico
  de 960 samples por pacote, soma `worker_drained_packets` e atualiza os
  contadores `worker_opus_*`; nao toca captura, bridge ou playback;
- `POST /api/audio/codec-v2/worker/stop` solicita parada, drena a fila
  restante, aguarda confirmacao e deixa `worker_state=stopped`;
- `POST /api/audio/codec-v2/worker/stress-test` executa teste diagnostico
  autocontido do worker Opus: limpa estado, inicia o worker opt-in, enfileira
  ate 40 pacotes sinteticos completos, espera drenar/codificar, para o worker
  e retorna deltas de pacotes, bytes Opus, drops, fila final e estado final;
  nao toca captura, bridge ou playback;
- `POST /api/audio/codec-v2/worker/feed-test` executa teste diagnostico
  autocontido do caminho `feed_pcm16` -> packetizer -> fila -> worker Opus:
  limpa estado, inicia o worker opt-in, alimenta ate 40 frames PCM16
  sinteticos de 960 samples pelo `audio_codec_service_v2_feed_pcm16()`,
  espera drenar/codificar, para o worker e retorna deltas de frames PCM,
  pacotes, bytes Opus, payloads observados, checksum/preview do ultimo payload,
  drops, fila final, pendencias e estado final; nao toca captura, bridge ou
  playback;
- o worker Opus agora observa o payload codificado localmente: a cada pacote
  drenado ele atualiza `worker_payload_packets`, `worker_payload_bytes_total`,
  `worker_payload_last_sequence`, `worker_payload_last_checksum` e uma preview
  fixa de ate 16 bytes em hex; isso e apenas diagnostico, nao existe fila de
  rede nem envio ao bridge;
- `reset` preserva o estado do worker quando ele esta ativo, evitando status
  incoerente ou segunda task acidental;
- o server expoe proxy diagnostico em `/api/device/audio/codec-v2`;
- o server tambem expoe `/api/device/audio/codec-v2/encode-test`;
- o server tambem expoe `/api/device/audio/codec-v2/drain`;
- o server tambem expoe `/api/device/audio/codec-v2/reset`;
- o server tambem expoe `/api/device/audio/codec-v2/opus-encode-test`;
- o server tambem expoe `/api/device/audio/codec-v2/overflow-test`;
- o server tambem expoe `/api/device/audio/codec-v2/worker/start`;
- o server tambem expoe `/api/device/audio/codec-v2/worker/stop`;
- o server tambem expoe `/api/device/audio/codec-v2/worker/stress-test`;
- o server tambem expoe `/api/device/audio/codec-v2/worker/feed-test`;
- CLI `noisebot_server debug codec-v2 status` consulta o endpoint do firmware;
- CLI `noisebot_server debug codec-v2 encode-test` aciona o teste sintetico;
- CLI `noisebot_server debug codec-v2 drain` drena a fila sintetica;
- CLI `noisebot_server debug codec-v2 reset` limpa o estado diagnostico;
- CLI `noisebot_server debug codec-v2 opus-encode-test` executa o encode Opus
  real diagnostico e isolado;
- CLI `noisebot_server debug codec-v2 overflow-test --packets N` executa o
  teste de overflow autocontido;
- CLI `noisebot_server debug codec-v2 worker-start` inicia o worker opt-in;
- CLI `noisebot_server debug codec-v2 worker-stop` para o worker opt-in;
- CLI `noisebot_server debug codec-v2 worker-stress-test --packets N` executa
  o teste multi-pacote autocontido do worker Opus;
- CLI `noisebot_server debug codec-v2 worker-feed-test --frames N` executa
  o teste autocontido do packetizer PCM16 com worker Opus opt-in;
- os endpoints nao ligam Opus persistente e nao alteram bridge/captura/playback;
- o Opus operacional existente permanece no caminho opt-in atual
  `/api/audio/opus/transport/enable`, com PCM16 como fallback padrao.

Validacao em hardware:

- apos flash em 2026-05-30, `GET /api/audio/codec-v2` retornou:
  `ok=true`, `initialized=false`, `format=pcm16`,
  `sample_rate_hz=16000`, `channels=1`, `opus_frame_ms=60`,
  `opus_frame_samples=960`, `opus_bitrate=32000`,
  `max_queue_packets=40`, `pcm_frames_in=0`, `packets_out=0`,
  `packet_drops=0`, `queue_count=0`, `error=ESP_OK`;
- `capture-v2 status` apos flash confirmou fallback seguro:
  `real_capture_enabled=false`, `session_active=false`,
  `state=IDLE_SESSION`, `last_error=ESP_OK`.
- CLI real `noisebot_server --host 192.168.1.30 debug codec-v2 status --json`
  retornou o mesmo contrato com `error=ESP_OK`.
- Validacao local do encode-test sintetico:
  - `bridge/tests`: 160 testes;
  - `server/tests`: 120 testes;
  - `idf.py build` limpo.
- Validacao local do packetizer PCM16 -> 960 samples:
  - `bridge/tests/test_firmware_audio_v2_skeleton_contract.py`: 6 testes;
  - `server/tests/test_server_facade.py`: 105 testes;
  - `bridge/tests`: 160 testes;
  - `server/tests`: 120 testes;
  - `idf.py build` limpo.
- Validacao local da fila sintetica limitada:
  - `bridge/tests/test_firmware_audio_v2_skeleton_contract.py`: 6 testes;
  - `server/tests/test_server_facade.py`: 105 testes;
  - `idf.py build` limpo.
- Validacao local do drain sintetico:
  - `bridge/tests/test_firmware_audio_v2_skeleton_contract.py`: 6 testes;
  - `server/tests/test_server_facade.py`: 107 testes;
  - `bridge/tests`: 160 testes;
  - `server/tests`: 122 testes;
  - `idf.py build` limpo.
- Validacao local do reset diagnostico:
  - `bridge/tests/test_firmware_audio_v2_skeleton_contract.py`: 6 testes;
  - `server/tests/test_server_facade.py`: 109 testes;
  - `bridge/tests`: 160 testes;
  - `server/tests`: 124 testes;
  - `idf.py build` limpo.
- Validacao local do overflow-test diagnostico:
  - `bridge/tests/test_firmware_audio_v2_skeleton_contract.py`: 6 testes;
  - `server/tests/test_server_facade.py`: 111 testes;
  - `bridge/tests`: 160 testes;
  - `server/tests`: 126 testes;
  - `idf.py build` limpo.
- Validacao local do stub de worker inativo:
  - `bridge/tests/test_firmware_audio_v2_skeleton_contract.py`: 6 testes;
  - `server/tests/test_server_facade.py`: 111 testes;
  - `bridge/tests`: 160 testes;
  - `server/tests`: 126 testes;
  - `idf.py build` limpo.
- Validacao local do encode Opus real diagnostico:
  - `bridge/tests/test_firmware_audio_v2_skeleton_contract.py`: 6 testes;
  - `server/tests/test_server_facade.py`: 113 testes;
  - `bridge/tests`: 160 testes;
  - `server/tests`: 128 testes;
  - `idf.py build` limpo.
- Validacao local do worker opt-in:
  - `bridge/tests/test_firmware_audio_v2_skeleton_contract.py`: 6 testes;
  - `server/tests/test_server_facade.py`: 115 testes;
  - `bridge/tests`: 160 testes;
  - `server/tests`: 130 testes;
  - `idf.py build` limpo.
- Validacao local do Opus dentro do worker opt-in:
  - `bridge/tests/test_firmware_audio_v2_skeleton_contract.py`: 6 testes;
  - `server/tests/test_server_facade.py`: 115 testes;
  - `bridge/tests`: 160 testes;
  - `server/tests`: 130 testes;
  - `idf.py build` limpo.
- Validacao local do worker Opus multi-pacote sintetico:
  - `bridge/tests/test_firmware_audio_v2_skeleton_contract.py`: 6 testes;
  - `server/tests/test_server_facade.py`: 116 testes;
  - `idf.py build` limpo.
- Validacao local do caminho feed PCM16 -> worker Opus:
  - `bridge/tests/test_firmware_audio_v2_skeleton_contract.py`: 6 testes;
  - `server/tests/test_server_facade.py`: 117 testes;
  - `idf.py build` limpo.
- Validacao local do observador de payload Opus do worker:
  - `bridge/tests/test_firmware_audio_v2_skeleton_contract.py`: 6 testes;
  - `server/tests/test_server_facade.py`: 117 testes;
  - `idf.py build` limpo;
- Validacao em hardware do observador de payload Opus do worker:
  - `worker-feed-test --frames 10` retornou `worker_payload_packets_delta=10`,
    `worker_payload_bytes_delta=2434`, `worker_payload_last_bytes=242`,
    `worker_payload_last_sequence=10`, `worker_payload_last_checksum>0`,
    `worker_payload_preview_len=16`, `worker_payload_preview_hex` nao vazio,
    `packet_drops_delta=0`, `queue_count_after=0`,
    `pending_samples_after=0`, `worker_state_after=stopped` e
    `error=ESP_OK`;
  - `codec-v2 status` e `capture-v2 status` apos um timeout transitorio
    responderam limpos: `opus_egress_queue_count=0`,
    `real_capture_enabled=false`, `session_active=false` e
    `last_error=ESP_OK`.
- Validacao local da fila egress Opus diagnostica:
  - `bridge/tests/test_firmware_audio_v2_skeleton_contract.py`: 6 testes;
  - `server/tests/test_server_facade.py`: 119 testes;
  - `idf.py build` limpo;
- Validacao em hardware da fila egress Opus diagnostica:
  - `worker-feed-test --frames 10` retornou `opus_egress_queue=true`,
    `opus_egress_packets_delta=10`, `opus_egress_bytes_delta=2434`,
    `opus_egress_packet_drops_delta=0`,
    `opus_egress_drained_after_test=10`,
    `opus_egress_queue_count_after_cleanup=0`,
    `opus_egress_last_bytes=242`, `opus_egress_last_checksum>0`,
    `opus_egress_preview_len=16`, `opus_egress_preview_hex` nao vazio,
    `packet_drops_delta=0`, `queue_count_after=0`,
    `pending_samples_after=0`, `worker_state_after=stopped` e
    `error=ESP_OK`;
  - `codec-v2 status` final confirmou `opus_egress_packets_in=10`,
    `opus_egress_packets_drained=10`,
    `opus_egress_packet_drops=0`, `opus_egress_queue_count=0`;
  - `capture-v2 status` final confirmou `real_capture_enabled=false`,
    `session_active=false`, `state=IDLE_SESSION` e `last_error=ESP_OK`.
- Stub de handoff Opus para bridge:
  - novo endpoint diagnostico `/api/audio/codec-v2/bridge-handoff-test`;
  - proxy server `/api/device/audio/codec-v2/bridge-handoff-test`;
  - CLI `noisebot_server debug codec-v2 bridge-handoff-test --frames N`;
  - internamente roda o mesmo caminho `feed_pcm16 -> worker Opus -> egress`,
    registra `bridge_handoff_*` como pacotes prontos para handoff, mas retorna
    `bridge_packet_not_sent=true` e `bridge_transport_unchanged=true`;
  - nao chama `bridge_service_send_opus_packet()`, nao renegocia HELLO, nao
    altera `bridge_service_set_opus_enabled()`, nao toca captura/playback e
    nao promove Opus como padrao;
  - validacao local: teste focado bridge 6, teste focado server 120 e
    `idf.py build`;
  - validacao em hardware apos flash:
    `noisebot_server --host 192.168.1.30 debug codec-v2 bridge-handoff-test --frames 10 --json`
    retornou `bridge_handoff_stub=true`,
    `bridge_transport_unchanged=true`, `bridge_packet_not_sent=true`,
    `bridge_handoff_packets_ready_delta=10`,
    `bridge_handoff_bytes_ready_delta=2434`,
    `opus_egress_queue_count_after_cleanup=0`, zero drops e
    `worker_state_after=stopped`;
  - `codec-v2 status` final confirmou `format=pcm16`,
    `worker_active=false`, `bridge_handoff_packets_ready=10`,
    `bridge_handoff_bytes_ready=2434`, `opus_egress_queue_count=0`,
    `packet_drops=0` e `error=ESP_OK`;
  - `capture-v2 status` final confirmou `real_capture_enabled=false`,
    `session_active=false`, `state=IDLE_SESSION` e `last_error=ESP_OK`.
- Transporte Opus live controlado pelo namespace Codec v2:
  - novos endpoints opt-in `/api/audio/codec-v2/transport/enable` e
    `/api/audio/codec-v2/transport/disable`;
  - proxies server `/api/device/audio/codec-v2/transport/enable` e
    `/api/device/audio/codec-v2/transport/disable`;
  - CLI `noisebot_server debug codec-v2 transport-enable --json` e
    `noisebot_server debug codec-v2 transport-disable --json`;
  - `transport-enable` inicia `audio_codec_service_v2_worker_start()` e entao
    liga `bridge_service_set_opus_enabled(true)`;
  - `audio_service` alimenta o worker v2 com PCM16 normalizado quando o
    transporte Opus esta habilitado;
  - o worker v2 agora codifica frames PCM reais de 960 samples e armazena
    pacotes Opus em fila egress real, drenada por
    `audio_codec_service_v2_read_opus_packet()` antes do envio por
    `bridge_service_send_opus_packet()`;
  - `transport-disable` desliga primeiro o transporte Opus do bridge e depois
    para o worker v2;
  - o JSON retorna `codec_v2_transport=true`,
    `transport_worker="audio_codec_service_v2"`,
    `compat_worker="audio_codec_service_v2"` e `pcm16_fallback=true`;
  - este passo nao troca o padrao para Opus, nao remove PCM16, nao altera wake,
    VAD, state machine, follow-up, captura v2 ou playback v2;
  - validacao local da migracao para worker v2 live: contrato bridge focado
    12, server facade 121 e `idf.py build` limpo;
  - validacao em hardware do worker v2 live apos flash:
    `codec-v2 status` inicial limpo, `transport-enable` retornou
    `transport_worker="audio_codec_service_v2"`, `opus_enabled=true` e
    `error=ESP_OK`, status ativo mostrou worker `running`,
    `opus_codec_error=0`, filas zeradas e zero drops;
  - turno real validado com transcript `Me conte uma história curta.`,
    `transcript_quality=good`, `outcome=llm`, `turn_id=4`,
    `chunk_count=39`, `total_samples=37424`, `duration_ms=2339.0`,
    `stt_ms=1088.0` e `first_audio_out_ms=5480.9`;
  - metricas do worker v2 no turno: `pcm_frames_in=39`, `packets_out=39`,
    `worker_drained_packets=39`, `worker_opus_packets=39`,
    `worker_opus_encoded_bytes_total=9488`,
    `opus_egress_packets_in=39`, `opus_egress_packets_drained=39`,
    `packet_drops=0`, `opus_egress_packet_drops=0`, `queue_count=0`,
    `opus_egress_queue_count=0`, `pending_samples=704` e
    `opus_codec_error=0`;
  - rollback: `transport-disable` retornou `live_bridge_transport=false`,
    `opus_enabled=false` e `error=ESP_OK`; status final confirmou
    `worker_active=false`, `worker_state=stopped`, fila zero e PCM16 como
    fallback;
  - `codec-ab` curto usando o worker v2 live validado em hardware:
    frase pareada `me diga uma curiosidade`; PCM16 retornou `ok=true`,
    `turn_id=7`, `outcome=local_intent`, transcript `Diga uma curiosidade.`,
    `transcript_similarity=0.858`, `stt_ms=1060.0` e `duration_ms=3200.0`;
    Opus v2 retornou `ok=true`, `turn_id=8`, `outcome=local_intent`,
    transcript `Me diga uma curiosidade.`, `transcript_similarity=1.0`,
    `stt_ms=1107.8`, `duration_ms=5219.0`, `packets_drained=87`,
    `packet_drops=0`, `encoded_bytes=21368` e
    `server_codec_confirmed=true`;
  - observacao de rollback do A/B: apos desabilitar Opus, o status mostrou
    1 pacote egress pendente, drenado manualmente por `codec-v2 egress-drain`;
    a correcao local faz `transport-disable` chamar
    `audio_codec_service_v2_drain_opus_egress()` e o harness `codec-ab`
    tambem chama `egress-drain` apos rollback;
  - validacao em hardware apos flash da correcao de rollback:
    `codec-ab --repeat 3 "me diga uma curiosidade"` passou com 3/3 PCM16 e
    3/3 Opus v2 `ok=true`; PCM16 teve STT medio ~1081.0 ms e todos os turnos
    com `transcript_quality=good`; Opus v2 teve STT medio ~1086.6 ms, todos
    com transcript `Me diga uma curiosidade.`, `transcript_similarity=1.0`,
    334 pacotes drenados pelo harness, 81213 bytes Opus e `packet_drops=0`;
  - status final apos a bateria confirmou `worker_active=false`,
    `worker_state=stopped`, `packet_drops=0`,
    `opus_egress_packet_drops=0`, `opus_egress_queue_count=0`,
    `opus_codec_error=0` e `capture-v2` desligado;
  - referencia de hardware anterior do primeiro controle live, ainda com
    worker de compatibilidade:
    `codec-v2 transport-enable` retornou `ok=true`,
    `live_bridge_transport=true`, `compat_worker="audio_processor_service"`,
    `pcm16_fallback=true` e `error=ESP_OK`;
  - status do worker de compatibilidade anterior confirmou `running=true`,
    `task_created=true`, `worker_ok=true`, `frame_samples=960`,
    `bitrate=32000`, `codec_error=0`, `last_error=ESP_OK`,
    `internal_after_open_kb=22` e stack em PSRAM observada por queda de PSRAM
    de 7173 KB para 7149 KB;
  - rollback do controle anterior:
    `codec-v2 transport-disable` retornou `live_bridge_transport=false`,
    `opus_enabled=false` e `error=ESP_OK`;
  - status final anterior confirmou worker parado, `capture-v2` sem sessao
    ativa, `real_capture_enabled=false`, Codec v2 limpo e `format=pcm16`.
- Validacao Opus live curta pelo namespace Codec v2 com worker de
  compatibilidade:
  - validacao em hardware apos flash:
    `noisebot_server --host 192.168.1.30 debug codec-v2 transport-enable --json`
    retornou `ok=true`, `codec_v2_transport=true`,
    `live_bridge_transport=true`, `compat_worker="audio_processor_service"`,
    `pcm16_fallback=true` e `error=ESP_OK`;
  - status do worker de compatibilidade confirmou `running=true`,
    `task_created=true`, `worker_ok=true`, `frame_samples=960`,
    `bitrate=32000`, `codec_error=0`, `last_error=ESP_OK`,
    `internal_after_open_kb=22` e stack em PSRAM observada por queda de PSRAM
    de 7173 KB para 7149 KB;
  - rollback imediato:
    `noisebot_server --host 192.168.1.30 debug codec-v2 transport-disable --json`
    retornou `live_bridge_transport=false`, `opus_enabled=false` e
    `error=ESP_OK`;
  - server rodando com `NOISEBOT_LLM_MODEL=qwen3.5:9b`;
  - `codec-v2 transport-enable` ligou `live_bridge_transport=true` e
    `opus_enabled=true`;
  - turno real produziu transcript `Fale uma frase curta.` com
    `transcript_quality=good`, `outcome=llm`, resposta do LLM e TTS;
  - metricas: `total_samples=51824`, `duration_ms=3239.0`, `stt_ms=1094.3`,
    `first_audio_out_ms=5490.9`, `tts_first_audio_ms=471.1`;
  - worker de compatibilidade: `pcm_encode_packets=54`,
    `opus_packet_enqueued=54`, `opus_packet_drained=54`,
    `opus_packet_drops=0`, `opus_packet_queue_count=0`,
    `opus_packet_bytes_total=13110`, `codec_error=0`;
  - rollback confirmou `live_bridge_transport=false`, `opus_enabled=false`,
    `capture-v2` desligado e status final em PCM16 limpo.
- Validacao em hardware do caminho feed PCM16 -> worker Opus apos flash:
  - `worker-feed-test --frames 10` retornou `ok=true`,
    `attempted_frames=10`, `attempted_samples=9600`,
    `pcm_frames_in_delta=10`, `packets_out_delta=10`,
    `worker_drained_packets_delta=10`, `worker_opus_packets_delta=10`,
    `worker_opus_encoded_bytes_delta=2434`,
    `worker_opus_last_packet_bytes=242`, `packet_drops_delta=0`,
    `queue_count_after=0`, `pending_samples_after=0`,
    `worker_state_after=stopped`, `error=ESP_OK`;
  - `capture-v2 status` apos o teste permaneceu com
    `real_capture_enabled=false`, `session_active=false`,
    `state=IDLE_SESSION` e `last_error=ESP_OK`.
- Validacao em hardware do worker Opus multi-pacote sintetico apos flash:
  - `worker-stress-test --packets 10` retornou `ok=true`,
    `accepted_packets=10`, `worker_drained_packets_delta=10`,
    `worker_opus_packets_delta=10`,
    `worker_opus_encoded_bytes_delta=2434`,
    `worker_opus_last_packet_bytes=242`, `packet_drops_delta=0`,
    `queue_count_after=0`, `worker_state_after=stopped`, `error=ESP_OK`;
  - `capture-v2 status` apos o teste permaneceu com
    `real_capture_enabled=false`, `session_active=false`,
    `state=IDLE_SESSION` e `last_error=ESP_OK`.
- Observacao de hardware do Opus dentro do worker opt-in:
  - a primeira tentativa com stack persistente igual ao teste temporario
    (`2048 * 12`) falhou em `worker-start` com HTTP 409 e
    `worker_state=error`, sem derrubar HTTP e sem tocar captura;
  - a tentativa seguinte com stack interna menor (`2048 * 6`) criou a task,
    mas o encode no worker tornou o HTTP indisponivel;
  - conclusao: o worker Opus precisa stack grande, mas ela nao deve consumir
    heap interno persistente;
  - correcao local: worker voltou para `2048 * 12`, agora criado com
    `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`
    e encerrado com `vTaskDeleteWithCaps`;
  - validacao local apos correcao PSRAM: teste focado bridge 6, server facade
    115 e `idf.py build` limpo;
  - validacao em hardware apos correcao PSRAM: `worker-start` retornou
    `ok=true`; `encode-test` com worker ativo gerou um pacote sintetico;
    status seguinte confirmou `queue_count=0`, `worker_drained_packets=1`,
    `worker_opus_packets=1`, `worker_opus_encoded_bytes_total=248`,
    `worker_opus_last_packet_bytes=248`, `packet_drops=0` e `error=ESP_OK`;
    `worker-stop` deixou `worker_active=false`, `worker_state=stopped`;
    `capture-v2 status` permaneceu com `real_capture_enabled=false` e
    `session_active=false`.
- Observacao de hardware: a primeira versao sincronamente no handler HTTP
  causou timeout e indisponibilidade HTTP apos o teste. A correcao moveu o
  encode para task temporaria `nb_codec_v2_opus_test` com stack proprio e
  timeout dedicado no CLI.
- Validacao em hardware do stub de worker inativo apos flash:
  - `GET /api/audio/codec-v2`: `initialized=false`, `format=pcm16`,
    `worker_supported=false`, `worker_active=false`,
    `worker_state=not_started`, contadores zerados e `error=ESP_OK`;
  - `GET /api/audio/capture-v2`: `real_capture_enabled=false`,
    `session_active=false`, `state=IDLE_SESSION`, `last_error=ESP_OK`.
- Validacao em hardware do overflow-test diagnostico apos flash:
  - `packets=40`: `accepted_packets=40`, `dropped_packets=0`,
    `peak_queue_count=40`, `queue_count_after_cleanup=0`,
    `status_packet_drops_after_cleanup=0`, `error=ESP_OK`;
  - `packets=41`: `accepted_packets=40`, `dropped_packets=1`,
    `peak_queue_count=40`, `queue_count_after_cleanup=0`,
    `status_packet_drops_after_cleanup=0`, `error=ESP_OK`;
  - `packets=45`: `accepted_packets=40`, `dropped_packets=5`,
    `peak_queue_count=40`, `queue_count_after_cleanup=0`,
    `status_packet_drops_after_cleanup=0`, `error=ESP_OK`;
  - status final do codec voltou limpo:
    `pcm_frames_in=0`, `packets_out=0`, `packet_drops=0`,
    `queue_count=0`, `pending_samples=0`, `format=pcm16`;
  - `capture-v2 status` confirmou fallback seguro:
    `real_capture_enabled=false`, `session_active=false`,
    `state=IDLE_SESSION`, `last_error=ESP_OK`.
- Validacao em hardware do reset diagnostico apos flash:
  - status inicial zerado e contrato fixo preservado;
  - apos `encode-test`: `pcm_frames_in=1`, `packets_out=1`,
    `packet_drops=0`, `queue_count=1`, `pending_samples=64`;
  - apos `reset`: `pcm_frames_in=0`, `packets_out=0`,
    `packet_drops=0`, `queue_count=0`, `pending_samples=0`,
    `format=pcm16`, `error=ESP_OK`;
  - status final manteve contadores zerados e contrato fixo;
  - `capture-v2 status` confirmou fallback seguro:
    `real_capture_enabled=false`, `session_active=false`,
    `state=IDLE_SESSION`, `last_error=ESP_OK`.
- Validacao em hardware do drain sintetico apos flash:
  - status inicial: `pcm_frames_in=0`, `packets_out=0`, `packet_drops=0`,
    `queue_count=0`, `pending_samples=0`, `error=ESP_OK`;
  - apos `encode-test`: `pcm_frames_in=1`, `packets_out=1`,
    `packet_drops=0`, `queue_count=1`, `pending_samples=64`,
    `error=ESP_OK`;
  - apos `drain`: `drained_packets=1`, `queue_count=0`,
    `pending_samples=64`, `packet_drops=0`, `error=ESP_OK`;
  - status final preservou `queue_count=0`, `pending_samples=64` e
    `error=ESP_OK`;
  - `capture-v2 status` confirmou fallback seguro:
    `real_capture_enabled=false`, `session_active=false`,
    `state=IDLE_SESSION`, `last_error=ESP_OK`.
- Validacao em hardware do `encode-test` apos flash:
  - `noisebot_server --host 192.168.1.30 debug codec-v2 encode-test --json`;
  - `ok=true`, `initialized=false`, `format=pcm16`,
    `pcm_frames_in=1`, `packets_out=1`, `packet_drops=0`,
    `queue_count=0`, `pending_samples=64`, `error=ESP_OK`.
- Validacao em hardware do worker opt-in apos flash:
  - status inicial: `worker_supported=true`, `worker_active=false`,
    `worker_state=not_started`, `queue_count=0`,
    `worker_drained_packets=0`, `error=ESP_OK`;
  - `worker-start`: `ok=true`, `worker_state=starting`, sem fila ou drops;
  - `encode-test` com worker ativo: `worker_active=true`,
    `worker_state=running`, `pcm_frames_in=1`, `packets_out=1`,
    `queue_count=1`, `worker_drained_packets=0`, `pending_samples=64`;
  - status seguinte: `worker_state=running`, `queue_count=0`,
    `worker_drained_packets=1`, `packet_drops=0`, `error=ESP_OK`;
  - `worker-stop`: `worker_active=false`, `worker_state=stopped`,
    `queue_count=0`, `worker_drained_packets=1`, `error=ESP_OK`;
  - `capture-v2 status`: `real_capture_enabled=false`,
    `session_active=false`, `state=IDLE_SESSION`, `last_error=ESP_OK`.
- Validacao em hardware do `opus-encode-test` corrigido apos flash:
  - `noisebot_server --host 192.168.1.30 debug codec-v2 opus-encode-test --json`;
  - `ok=true`, `test_format=opus`, `frame_samples=960`,
    `encoded_bytes=248`, `codec_error=0`, `opus_encode_tests=1`,
    `opus_last_packet_bytes=248`, `queue_count=0`, `packet_drops=0`,
    `worker_active=false`, `worker_state=stopped`, `error=ESP_OK`;
  - status seguinte confirmou HTTP saudavel, `opus_encoded_bytes_total=248`,
    fila zerada e `opus_codec_error=0`;
  - `capture-v2 status`: `real_capture_enabled=false`,
    `session_active=false`, `state=IDLE_SESSION`, `last_error=ESP_OK`.

Aceite:

- `opus-live` ok.
- `codec-ab` curto ok.
- packet drops = 0.
- PCM16 fallback intacto.

### Fases Restantes Pos-Opus

Status: movidas para `docs/VOICE_AUDIO_V2_NEXT_PHASES.md` para evitar que o
fechamento do Opus fique misturado com o restante da decomposicao de audio.

Resumo:

- Fase I: `audio_playback_service_v2` assume gradualmente o downlink
  SAY/playback, com cancel/drain/status e sem HAL direto ate o handoff. A fila
  SAY real ja saiu do `audio_service` e passou para
  `audio_playback_service_v2`; o `audio_service` ainda drena os chunks e escreve
  no speaker para preservar o ownership seguro do HAL. Validacao pos-restart
  em hardware confirmou Opus ativo, turno `local_time`, barge-in por wake
  durante fala longa, fila final zero, cancelamento p50 2,6 ms / p95 3,2 ms e
  `ESP_OK`.
- Fase M parcial: checklist/health de release em
  `docs/VOICE_AUDIO_V2_RELEASE_CHECKLIST.md`, protegendo Opus v2, Playback v2
  dono da fila SAY, Capture v2 desligado, barge/no-echo e completude TTS/texto,
  sem alterar firmware C.
- Fase J: `voice_activity_service_v2` entra como processor shadow/opt-in para
  VAD/NS/AFE, sem AEC device-side no hardware atual. Primeiro passo local:
  shadow probe passivo em `/api/audio/activity-v2`, alimentado por copia de
  PCM do `audio_service` e sem posse de HAL/bridge/wake/captura.
- Fase K: `voice_capture_session_v2` assume gradualmente pre-roll, timeouts,
  discard reasons e `VOICE_START/AUDIO_CHUNK/VOICE_END` por flag.
- Fase L: policy conversacional avancada, incluindo follow-up opt-in e
  turn-taking mais natural, so depois de no-echo/captura estaveis.
- Fase M: checklist/health/replay de release para preservar Opus, PCM16,
  playback, texto visual e turn-taking.

As restricoes continuam as mesmas: PCM16 e rollback permanecem obrigatorios,
wake word atual nao muda sem evidencia, AEC device-side segue bloqueado sem
referencia limpa de playback, e barge-in sem wake/follow-up automatico nao
entram junto com refactor de audio.

## Checklist de Testes por Fase

Automatizados:

- `bridge/tests`;
- `server/tests`;
- contrato de HELLO PCM16/Opus;
- fake firmware PCM16/Opus;
- codec round-trip;
- session replay;
- tests de cancel/queue quando criados.

Hardware/manual assistido:

- `noisebot_server debug codec-ab`;
- `noisebot_server debug opus-live`;
- `noisebot_server debug capture-v2`;
- `noisebot_server debug barge-live`;
- `noisebot_server debug no-echo-live`;
- `noisebot_server debug opus-quality`;
- `docs/VOICE_AUDIO_V2_RELEASE_CHECKLIST.md` para gates de release local,
  incluindo `codec-v2 health`, Playback v2 SAY, Capture v2 desligado,
  rollback PCM16, barge/no-echo e completude TTS/texto;
- teste ambiente real:
  - TV ligada;
  - conversa distante;
  - fala normal perto;
  - fala baixa;
  - barge-in durante TTS;
  - silencio apos wake.

Metricas obrigatorias:

- `turn_id`;
- codec;
- transcript;
- transcript similarity;
- total samples;
- duration ms;
- STT ms;
- packet drops;
- queue count;
- encoded bytes;
- wake accepted/rejected;
- VAD start/end;
- reason de fim da sessao;
- heap internal/DMA/PSRAM quando codec/AFE ativo.

## Decisoes Ja Tomadas

- PCM16 continua fallback obrigatorio e rollback operacional.
- Opus v2 continua capability opt-in no contrato do firmware, mas esta
  promovido como default local do server em `server/.env` por
  `NOISEBOT_AUDIO_DEFAULT_CODEC=opus-v2`.
- Opus upstream usa 16 kHz mono, 60 ms, 32 kbps.
- AEC device-side nao e promovivel no hardware atual sem referencia de playback.
- Follow-up automatico fica em standby ate a base de audio v2 estar estavel.
- Barge-in sem wake por VAD fica fora ate AEC/AFE e no-echo estarem robustos.
- Nao trocar o bridge TCP inteiro agora.
- Nao usar C++/STL fora das excecoes ja permitidas pelo projeto.

## Fechamento da Migracao Opus v2

Status: concluida como default local, com rollback PCM16 preservado.

O worker live do proprio `audio_codec_service_v2` ja assumiu o transporte Opus
opt-in no lugar do worker de compatibilidade, mantendo PCM16 como padrao e
rollback via `codec-v2 transport-disable`. A validacao local passou com
contrato bridge focado, server facade e `idf.py build`; a validacao em
hardware tambem passou com turno Opus live curto, A/B PCM16 vs Opus v2,
bateria de 3 turnos por codec e regressao `barge-live`/`no-echo-live` com
Opus v2 opt-in. O contrato HELLO agora promove Opus v2 como capability oficial
opt-in sem mudar o default: `codecs` continua representando o transporte ativo
e permanece `pcm16=true`, `opus=false` no HELLO padrao; `codec_options`
anuncia suporte a `opus_tx`, `opus_default=false`, 16 kHz mono, 60 ms/960
samples e 32 kbps. O server tambem espelha `codec_options` em `/api/ai/status`.
O proximo avanco seguro e flashar essa mudanca de contrato e validar no
hardware que HELLO/status continuam PCM16 por padrao, anunciam a capability
Opus v2 opt-in e mantem rollback limpo. Validacao apos flash passou: o server
foi reiniciado com `qwen3.5:9b`, `/ai/status` mostrou `audio.format=pcm16`,
`codecs={"pcm16":true,"opus":false}` e `codec_options.opus_tx=true` com
`opus_default=false`; `codec-v2 transport-enable` alternou o status para Opus
ativo, `transport-disable` voltou para PCM16 e o status final confirmou worker
parado, fila egress zero, zero drops, `opus_codec_error=0` e `capture-v2`
desligado.

Avanco de promocao configuravel: o server agora aceita
`NOISEBOT_AUDIO_DEFAULT_CODEC=pcm16|opus-v2` e o CLI `--audio-codec
pcm16|opus-v2`. O default continua `pcm16`, mas quando iniciado com
`--audio-codec opus-v2` o server chama `codec-v2 transport-enable` no startup,
faz o HELLO/status mudarem para Opus ativo e preserva rollback por
`transport-disable` ou por reinicio sem a flag. Validacao local passou com
`server/tests`; validacao live confirmou startup em Opus v2, worker rodando,
server em `qwen3.5:9b`, e rollback final para PCM16 com worker parado, fila
zero e `capture-v2` desligado.

Soak real com `--audio-codec opus-v2`: usuario executou varios testes com
intents locais e LLM. Estado pos-uso confirmou server conectado, Opus ativo,
ultimo turno LLM sem erro, `pcm_frames_in=579`, `packets_out=579`,
`worker_opus_packets=579`, `opus_egress_packets_drained=579`,
`worker_opus_encoded_bytes_total=140756`, `packet_drops=0`,
`opus_egress_packet_drops=0`, `opus_egress_queue_count=0`, worker `running`,
`opus_codec_error=0` e `capture-v2` desligado.

Promocao local aplicada: `server/.env` agora define
`NOISEBOT_AUDIO_DEFAULT_CODEC=opus-v2`. O server foi reiniciado sem
`--audio-codec` e mesmo assim `/ai/status` confirmou Opus ativo, modelo
`qwen3.5:9b`, worker v2 `running`, `packets_out=738`,
`opus_egress_packets_drained=738`, zero drops, fila egress zero,
`opus_codec_error=0` e `capture-v2` desligado. Rollback local: remover/alterar
essa env para `pcm16` ou chamar `codec-v2 transport-disable`.

Diagnostico de corte de resposta: o server agora registra por turno contadores
de TTS/playback em `recent_voice_sessions`: frases e chars enviados ao TTS,
chunks SAY enviados, bytes PCM recebidos/enviados, padding, `SAY_BEGIN`,
`SAY_END`, duracao esperada de fala, `tts_completed` e truncamento do
`TEXT_SCROLL` de 128 bytes. Isso nao muda firmware nem protocolo; serve para
separar corte real de audio, limite visual de texto e falha de envio ao
firmware. Validacao local: `server/tests` com 143 testes verdes.

O diagnostico foi promovido para analise automatica em `/ai/metrics`: se
`tts_completed=false`, `voice_alert` marca "Fala possivelmente incompleta" e
orienta checar chunks SAY, `SAY_BEGIN/SAY_END` e cancelamentos; se apenas
`text_scroll_truncated=true`, `voice_diagnosis` indica limite visual de
`TEXT_SCROLL`, sem tratar isso como corte de audio. Validacao local:
`server/tests` com 145 testes verdes.

Validacao real apos resposta longa confirmou `tts_completed=true`,
`tts_say_end_sent=true`, 589 chunks SAY, `tts_expected_duration_ms=9424.0`,
`voice_alert=null` e `voice_diagnosis` apontando apenas truncamento visual de
`TEXT_SCROLL`. Para evitar falso diagnostico nos proximos testes, o
`last_voice_session.reply` em `/ai/metrics` agora preserva ate 1200 caracteres,
enquanto `transcript` preserva ate 500. Validacao local: `server/tests` com
146 testes verdes.

Texto visual longo agora e paginado no server sem novo opcode: a resposta e
dividida em paginas UTF-8 seguras de ate 128 bytes e enviada como multiplos
`TEXT_SCROLL` espacados durante a fala. O firmware atual continua recebendo
frames `TEXT_SCROLL` compatíveis; portanto esta mudanca nao exige novo
protocolo binario e nao altera audio/Opus/PCM16. As metricas registram
`text_scroll_pages` e `text_scroll_pages_sent`, e o diagnostico de
`/ai/metrics` diferencia truncamento antigo de paginacao concluida. Validacao
local: `server/tests` com 149 testes verdes.

Refino de largura visual: uma resposta curta/media ainda podia caber em uma
unica pagina de 128 bytes e mesmo assim terminar antes do overlay horizontal do
firmware mostrar tudo. O server agora divide paginas tambem por limite visual
aproximado de 38 caracteres, mantendo o limite UTF-8 de 128 bytes e o mesmo
opcode `TEXT_SCROLL`. Isso e server-only, nao exige flash, nao altera TTS,
Opus, PCM16 nem sincronismo de audio. Validacao local: teste focado de
paginacao e `server/tests` com 150 testes verdes.

Guardrail operacional do Codec v2: o server agora tem diagnostico
`codec-v2 health`, tambem exposto em `/api/device/audio/codec-v2/health`, que
le o status do firmware e classifica `ok`, `warn` ou `degraded` a partir de
drops, `opus_codec_error`, worker ativo/estado, fila pronta e fila egress. O
retorno inclui `issues`, `warnings` e hint de rollback para
`codec-v2 transport-disable` ou `NOISEBOT_AUDIO_DEFAULT_CODEC=pcm16`. Isso nao
altera firmware, protocolo, captura, playback, TTS, Opus nem PCM16; e apenas
observabilidade para evitar confundir fila/drops de codec com STT/LLM/TTS.
Validacao local: teste focado de health/CLI e `server/tests` com 154 testes
verdes. Validacao live: health primeiro apontou `opus_egress_queue_count=1`
sem drops/erro; `codec-v2 egress-drain` drenou 1 pacote e health voltou
`status=ok`, `healthy=true`, zero drops, fila egress zero e
`opus_codec_error=0`.

Validacao de fechamento:

- `codec-v2 health` retornou `healthy=true`, `status=ok`, `issues=[]`,
  `warnings=[]`, `packet_drops=0`, `opus_egress_packet_drops=0`,
  `opus_egress_queue_count=0`, `opus_codec_error=0` e worker `running`.
- `/ai/status` confirmou server conectado, modelo `qwen3.5:9b` e audio ativo
  em Opus 16 kHz mono, 60 ms.
- Server local continua com `NOISEBOT_AUDIO_DEFAULT_CODEC=opus-v2`.
- Server local tambem precisa de `NOISEBOT_HOST=192.168.1.30` ou `--host
  192.168.1.30`; sem isso, o processo pode subir sem transporte e aparecer como
  `connected=false`, apesar da Ops API responder.
- Rollback documentado: mudar env para `pcm16`, reiniciar server, ou chamar
  `codec-v2 transport-disable` para voltar o transporte ativo para PCM16.

Fora do fechamento:

- AEC device-side continua bloqueado no hardware atual sem referencia limpa de
  playback.
- Follow-up automatico permanece desligado.
- Barge-in sem wake por VAD permanece fora ate AFE/AEC/no-echo terem criterio
  proprio.
- Refatorar completamente `audio_service` para separar Audio I/O, playback,
  VAD e capture session ainda e fase futura; o fechamento aqui e da migracao
  de transporte Opus v2.

Qualquer mudanca em wake, VAD thresholds, state machine, barge-in ou follow-up
antes disso deve ser considerada fora de escopo.
