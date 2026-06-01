---
title: NoiseBot Voice Audio v2 Knowledge Base
created: 2026-05-30
status: active-reference
project: NoiseBot
tags:
  - noisebot
  - voice
  - audio-v2
  - esp32s3
  - opus
  - vad
  - aec
  - firmware
---

# NoiseBot Voice Audio v2 Knowledge Base

Esta nota e feita para Obsidian e para consulta por IAs futuras. Ela resume o
conhecimento tecnico consolidado sobre a migracao de voz do NoiseBot, com base
no firmware, server, bridge, testes locais e validacoes em hardware do proprio
projeto.

Use esta nota como entrada rapida antes de alterar qualquer parte de audio,
captura, reproducao, VAD, Opus, bridge, wake word ou barge-in.

## Leitura Obrigatoria

- [[VOICE_AUDIO_V2_ARCHITECTURE]]
- [[VOICE_AUDIO_V2_NEXT_PHASES]]
- [[VOICE_AUDIO_V2_RELEASE_CHECKLIST]]
- [[VOICE_PIPELINE]]
- [[ROADMAP]]
- [[ARCHITECTURE]]
- [[HARDWARE]]

Arquivos fonte centrais no NoiseBot:

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

## Resumo Executivo

O NoiseBot precisa refazer o subsistema de voz em uma arquitetura v2 paralela,
sem quebrar o pipeline atual. O problema principal nao e apenas Opus. O problema
e que captura, playback, VAD, pre-roll, bridge, Opus, AFE shadow, diagnostico e
recuperacao I2S estao concentrados demais no `audio_service.c`.

Meta da arquitetura v2:

- Separar Audio I/O, playback, voice activity, capture session, codec e bridge.
- Preservar PCM16 como fallback padrao.
- Manter Opus como capability opt-in no firmware e default local do server apos
  validacao, sempre com rollback PCM16.
- Manter wake word e barge-in atuais intactos enquanto a base v2 nasce.
- Evoluir por fases pequenas, com testes locais, hardware e rollback claro.

Regra principal:

> Nao consertar wake, VAD, Opus, AEC e playback ao mesmo tempo.

## Decisoes Fixadas

### PCM16

- Continua sendo fallback obrigatorio e rollback operacional.
- Nao remover.
- O default local do server agora e Opus v2 via
  `NOISEBOT_AUDIO_DEFAULT_CODEC=opus-v2`, mas PCM16 continua disponivel por
  env `pcm16` ou `codec-v2 transport-disable`.

### Opus

- Continua capability opt-in no contrato do firmware, mas esta concluido como
  default local do server.
- Perfil upstream adotado:
  - sample rate: 16000 Hz;
  - mono;
  - frame: 60 ms;
  - frame samples: 960;
  - bitrate: 32000 bps;
  - application: audio;
  - complexity: 0;
  - FEC: off;
  - DTX: on;
  - VBR: on.
- 60 ms foi mantido porque funcionou bem para latencia, overhead e filas
  curtas no NoiseBot.
- 32 kbps vem do diagnostico offline nos WAVs reais do NoiseBot.

### Proximas Fases Pos-Opus

O detalhamento operacional fica em `docs/VOICE_AUDIO_V2_NEXT_PHASES.md`.

Ordem atual:

1. Playback v2 como dono gradual do downlink SAY/playback.
2. Checklist/health de release para preservar Opus, PCM16, texto visual e
   turn-taking.
3. Voice Activity v2 em shadow/opt-in para VAD/NS/AFE, sem AEC device-side.
4. Capture Session v2 assumindo upstream por flag.
5. Policy conversacional avancada apenas depois de no-echo/captura estaveis.

Regra: nao reabrir wake threshold, follow-up automatico, AEC ou barge-in sem
wake dentro da mesma mudanca de playback/captura/codec.

Fase M parcial:

- `docs/VOICE_AUDIO_V2_RELEASE_CHECKLIST.md` fixa gates de release local sem
  alterar firmware C.
- Proteger antes de novos refactors: Opus v2 por `codec-v2 health`, Playback v2
  dono da fila SAY, Capture v2 desligado, PCM16 rollback, `barge-live`,
  `no-echo-live` e completude TTS/texto.
- Fora do escopo: wake, VAD, AEC, follow-up, `audio_service.c` e HAL.

Incremento atual da Fase I:

- Playback v2 ja assumiu a fila curta estatica do downlink SAY real
  (`bridge_say_queue_owner=true`), enquanto `audio_service` continua como dono
  seguro do HAL/speaker e drena a fila pelo contrato v2.
- `/api/audio/playback-v2` expoe contadores `say_*` para recebidos, tocados,
  drops, drops durante escuta, cancelamentos, profundidade de fila e ownership
  da fila.
- Validacao de hardware fechou o incremento: turno real teve 283 chunks SAY
  recebidos/tocados, fila final zero, zero drops e `SAY_END`; rodada
  controlada pos-restart confirmou Opus ativo, `Que horas são?` como
  `local_time`, barge-in em historia longa com `discard_reason=barge_in`,
  cancelamento p50 2,6 ms / p95 3,2 ms, `say_queue_count=0`,
  `say_cancel_count=2`, `say_chunks_cancelled=28` e `ESP_OK`.

### Wake Word

- Nao refazer agora.
- Nao ajustar threshold por impulso.
- Wake word atual voltou a funcionar e deve ser preservada enquanto v2 nasce.
- Wake em IDLE e wake durante RESPONDING sao casos diferentes.

### Barge-in

- O caminho aceito e barge-in por wake word durante resposta.
- Barge-in por fala sem wake fica fora ate AEC/AFE/no-echo estarem robustos.
- Teste obrigatorio quando mexer em sessao/playback:
  `noisebot_server debug barge-live "me conte uma historia longa" --json`

### Follow-up

- Follow-up automatico fica em standby.
- Nao reativar junto com audio v2 inicial.
- Risco: robo ouvir conversa ambiente apos responder.

### AEC

- NoiseBot atual nao tem canal limpo de referencia de speaker.
- AEC device-side nao deve ser promovido no hardware atual.
- AFE pode ser usado para VAD/NS sem prometer AEC.
- Server-side AEC so depois de timestamps de playback e desenho explicito.

## Principios Operacionais

- I/O de audio separado de codec e de policy.
- Task dedicada para codec Opus, com filas curtas.
- Processor plugavel entre microfone e codec.
- Estados explicitos de conversa: idle, listening, speaking, abort/cancel.
- Abort/cancel limpo quando wake acontece durante fala.
- Capabilities de audio explicitas no protocolo.
- AEC device-side bloqueado enquanto nao houver referencia limpa de playback.
- Firmware C17, com C++ restrito as excecoes ja permitidas pelo projeto.

## Estado Atual do NoiseBot

### `audio_hal`

Responsabilidade atual:

- I2S0 full-duplex.
- RX INMP441.
- TX MAX98357A.
- Conversao mono logica.
- Chunk base: 256 samples / 16 ms.

Preservar:

- API de baixo nivel.
- DMA em SRAM.
- I2S recovery pode ser melhorado, mas nao misturar com VAD.

### `audio_service`

Hoje faz coisa demais:

- playback WAV;
- playback SAY;
- silencio TX;
- mic read;
- high-pass;
- sound analysis;
- ESP-SR VAD;
- heuristica RMS/ZCR/espectral;
- pre-roll;
- wake feed;
- AFE shadow feed;
- sessao de escuta;
- bridge TX;
- Opus feed/drain;
- diagnostico WAV;
- I2S recovery.

Este e o principal alvo de decomposicao.

### `audio_processor_service`

Hoje mistura:

- AFE probe;
- AEC probe;
- shadow processor;
- fonte processada para bridge;
- Opus worker;
- fila de pacotes Opus.

Separar no v2:

- AFE/VAD/NS em `voice_activity_service_v2`.
- Codec em `audio_codec_service_v2`.
- Probes como diagnostico, nao caminho critico.

### `bridge_service`

Preservar:

- contrato TCP atual;
- `VOICE_START`;
- `AUDIO_CHUNK`;
- `VOICE_END`;
- SAY;
- session events;
- HELLO/capabilities.

Nao colocar no bridge:

- VAD;
- ganho de mic;
- regra de wake;
- regra de fim de fala.

### `voice_controller`

Preservar:

- politica de wake;
- politica de barge-in;
- decisao de abrir listen;
- integracao com state machine.

Nao colocar nele:

- DSP;
- codec;
- VAD detalhado;
- ajuste de audio.

## Arquitetura V2 Desejada

```text
audio_hal
  -> audio_io_service_v2
      -> wake_service atual
      -> voice_activity_service_v2
      -> voice_capture_session_v2
          -> audio_codec_service_v2
              -> bridge_service
      <- audio_playback_service_v2
          <- bridge_service SAY
          <- assets locais/synth
```

### Componentes V2

#### `audio_io_service_v2`

Faz:

- ler mic;
- escrever speaker/silencio;
- manter full-duplex;
- normalizar PCM16;
- expor frames internos;
- recuperar I2S.

Nao faz:

- VAD;
- bridge;
- Opus;
- state machine;
- wake policy.

#### `audio_playback_service_v2`

Faz:

- WAV local;
- PCM raw local;
- SAY do bridge;
- fila SAY estatica de 16 chunks;
- enqueue/dequeue/cancel/drain;
- volume;
- cancel/stop;
- descarte de fila velha.
- O `OutputScheduler` do server deve enviar SAY com prebuffer curto e depois
  cadencia real de 16 ms por chunk. Pausas entre sentencas do TTS nao devem
  gerar rajadas de catch-up, porque isso enche a fila SAY e causa engasgos
  mesmo com `tts_completed=true` e `SAY_END`.
- Validacao real da correcao de pacing: apos restart do server, uma resposta
  curta gerou 398 chunks TTS e `SAY_END`; `/api/audio/playback-v2` saiu de
  `received=494/played=494/dropped=154` para
  `received=892/played=892/dropped=154`, confirmando zero drops novos.

Nao faz:

- HAL/speaker direto neste incremento;
- captura;
- VAD;
- STT;
- wake.

#### `voice_activity_service_v2`

Faz:

- shadow probe passivo em `/api/audio/activity-v2`;
- receber copia de PCM condicionado do `audio_service`, sem HAL proprio;
- medir RMS/peak/ZCR, fala/silencio, frames mutados e sessao ativa;
- separar telemetria por contexto (`session_frames`, `idle_frames`, frames
  mutados/nao mutados e maximos RMS/peak/ZCR por bucket) para comparar fala real
  e vazamento de playback sem promover decisao ativa;
- aceitar shadow de ate 30 s e receber contexto explicito de playback do
  `audio_service` apenas para bucket de telemetria;
- validacao em hardware do shadow 30 s: 1875 frames em 30000 ms,
  `session_frames=268`, `idle_frames=1607`, `muted_frames=478`,
  `unmuted_frames=1397`, Opus v2 saudavel, Capture v2 desligado; ponto amarelo
  separado em Playback v2 com 14 drops SAY no turno;
- ESP-SR VAD primario;
- RMS/ZCR/espectral como telemetria;
- AFE/NS opcional;
- eventos internos speech/silence.

Nao faz:

- abrir sessao em IDLE;
- mandar bridge;
- decidir wake.
- tocar playback ou mudar codec/captura.

#### `voice_capture_session_v2`

Faz:

- espera por fala;
- captura;
- ending on silence;
- timeout;
- pre-roll;
- `VOICE_START`;
- `VOICE_END`;
- razao de descarte.

Nao faz:

- codec;
- DSP pesado;
- playback.

#### `audio_codec_service_v2`

Faz:

- PCM16 passthrough;
- Opus encode;
- filas curtas;
- metricas;
- worker dedicado.

Nao faz:

- VAD;
- wake;
- playback;
- bridge policy.

## Contrato de Sessao

Estados internos desejados:

```text
IDLE_SESSION
WAITING_FOR_SPEECH
CAPTURING
ENDING_ON_SILENCE
CANCELLED
DONE
```

Regras:

- Wake valido abre sessao.
- Barge-in valido abre sessao com pre-roll suprimido.
- Follow-up so se estiver explicitamente habilitado.
- VAD nao abre sessao sozinho.
- Wake vazio nao envia audio ao STT.
- `VOICE_END` so existe se `VOICE_START` e audio existiram.
- Cancel limpa filas de audio pendente.

Parametros iniciais:

```text
wait_for_speech = 8000 ms
end_silence = 900 ms
max_speech = 9200 ms
pre_roll = 20 * 256 samples = 320 ms
chunk_base = 256 samples = 16 ms
opus_frame = 960 samples = 60 ms
```

## Contrato de Playback

Regras:

- Speaker sempre recebe audio ou silencio.
- SAY do bridge usa fila curta.
- Stop/cancel deve limpar fila SAY.
- Durante escuta, chunks SAY antigos devem ser descartados.
- Playback nao pode virar fala do usuario.
- Janela de mute pos-playback deve proteger VAD.

Casos obrigatorios:

- TTS normal termina e volta a IDLE.
- Wake durante TTS interrompe e abre barge-in.
- Chunks SAY recebidos depois do cancel nao tocam.
- Fila cheia gera drop metricado, nao crash.

## Contrato de Codec

PCM16:

- padrao;
- direto para bridge;
- fallback de rollback;
- referencia para comparacao de qualidade.

Opus:

- opt-in;
- `frame_duration=60`;
- `chunk_samples=960`;
- `bitrate=32000`;
- fila curta;
- packet drops metricados;
- server decodifica para PCM antes do STT.

Nunca:

- tornar Opus padrao sem A/B live suficiente.
- corrigir problema de VAD alterando Opus.
- corrigir problema de Opus alterando wake.

## Matriz de Riscos

| Sintoma | Suspeita | Onde olhar primeiro | Nao fazer |
| --- | --- | --- | --- |
| Hi ESP detecta mas nao escuta fala | sessao/VAD/pre-roll | capture session, VAD state, VOICE_START | mexer em wake threshold |
| Robo responde conversa ambiente | VAD/follow-up abrindo sem wake | session open source, follow-up flag | baixar threshold |
| Barge-in corta mas responde "Oi" | audio curto/vazio apos cancel | pre-roll, capture start, STT discard | reativar follow-up |
| TTS velho toca depois do cancel | fila SAY nao limpa | playback queue, bridge SAY drop | mexer no STT |
| Opus piora transcript | codec/janela/volume | codec-ab, packet drops, duration | mexer no VAD sem evidencia |
| Resposta falada corta antes do fim | TTS/streaming/chunk/playback queue | SAY chunks, TTS stop, playback queue, text length | culpar Opus sem A/B |
| Resposta falada engasga mas TTS completou | rajada SAY por pacing server-side ou fila firmware cheia | `say_chunks_dropped`, `tts_chunks_sent`, `tts_completed`, `SAY_END` | aumentar fila ou culpar Opus antes de checar pacing |
| AEC probe falha | sem referencia/heap | processor status | forcar AEC device-side |
| Crash I2S/ISR | I/O/recovery | audio_hal, task stack, DMA | adicionar processamento no ISR |
| STT audio_curto | VOICE_START/END errado | session state, total_samples | culpar LLM |

Pendencia observada em uso real:

- Algumas respostas faladas parecem cortar o texto antes do fim. Investigar
  depois da migracao Opus, tratando como problema separado de TTS/streaming,
  chunking, fila de playback ou criterio de fim de fala. Nao atribuir ao codec
  sem evidencia de A/B.

## Comandos de Teste

Server tests:

```powershell
cmd.exe /c "set PYTHONPATH=D:\Projetos\Noisebot\server&& C:\Users\Tauser\AppData\Local\Python\pythoncore-3.14-64\python.exe -m pytest server\tests"
```

Bridge tests:

```powershell
cmd.exe /c "set PYTHONPATH=D:\Projetos\Noisebot\bridge&& C:\Users\Tauser\AppData\Local\Python\pythoncore-3.14-64\python.exe -m pytest bridge\tests"
```

Firmware build:

```powershell
cmd.exe /c "set IDF_PYTHON_ENV_PATH=C:\Users\Tauser\.espressif\python_env\idf5.5_py3.14_env&& call C:\esp\v5.5.4\esp-idf\export.bat && idf.py build"
```

Opus quality offline:

```powershell
python -m noisebot_server debug opus-quality --input D:\Projetos\Noisebot\voice_samples --json
```

Codec A/B live:

```powershell
python -m noisebot_server --host 192.168.1.30 debug codec-ab --phrases "me diga uma curiosidade" "que horas sao" --json
```

Barge-in:

```powershell
python -m noisebot_server --host 192.168.1.30 debug barge-live "me conte uma historia longa" --json
python -m noisebot_server --host 192.168.1.30 debug barge-live "me conte uma historia longa" --codec opus-v2 --json
```

No echo:

```powershell
python -m noisebot_server --host 192.168.1.30 debug no-echo-live "me conte uma historia longa" --json
python -m noisebot_server --host 192.168.1.30 debug no-echo-live "me conte uma historia longa" --codec opus-v2 --json
```

Capture v2 status/replay:

```powershell
python -m noisebot_server --host 192.168.1.30 debug capture-v2 status --json
python -m noisebot_server --host 192.168.1.30 debug capture-v2 replay --speech-ms 640 --silence-ms 900 --json
python -m noisebot_server --host 192.168.1.30 debug capture-v2 live --json
```

Release local Voice Audio v2:

```powershell
python -m noisebot_server --host 192.168.1.30 debug codec-v2 health --json
python -m noisebot_server --host 192.168.1.30 debug capture-v2 status --json
python -m noisebot_server --host 192.168.1.30 debug codec-ab --repeat 3 "me diga uma curiosidade" --json
python -m noisebot_server --host 192.168.1.30 debug barge-live "me conte uma historia longa" --codec opus-v2 --json
python -m noisebot_server --host 192.168.1.30 debug no-echo-live "me conte uma historia longa" --codec opus-v2 --json
```

## Criterios de Aceite por Etapa

### Esqueleto v2 inativo

Status: concluido em `e7dfea2`.

- Build limpo.
- Nenhum comportamento alterado.
- Nenhum componente v2 inicializado no boot.
- Teste confirma v1 ativo.
- Componentes criados:
  `audio_io_service_v2`, `audio_playback_service_v2`,
  `voice_activity_service_v2`, `voice_capture_session_v2` e
  `audio_codec_service_v2`.

Validacao:

- `idf.py build`.
- `bridge/tests`: 156 testes.

### Audio I/O probe

Status: iniciado em `563fd3c`.

- Probe HTTP explicito:
  - `GET /api/audio/io-v2`;
  - `POST /api/audio/io-v2/probe`;
  - `POST /api/audio/io-v2/probe/stop`.
- Le mic de forma passiva, por espelhamento do PCM16 ja lido pelo
  `audio_service`.
- Alimenta/contabiliza speaker com silencio somente quando o loop v1 ja esta
  escrevendo silencio.
- Nao toca wake.
- Nao toca bridge.
- Nao disputa `audio_hal` nem cria task v2.
- Exibe metricas de RX, TX silencio, drops, RMS/peak e heap.

Validacao:

- `idf.py build`.
- `bridge/tests`: 157 testes.
- Firmware real em 2026-05-30:
  - OTA aplicado com build `abc9400`;
  - `duration_ms=1000`;
  - `rx_frames=63`;
  - `tx_silence_frames=63`;
  - `probe_elapsed_ms=1008`;
  - `dropped_frames=0`;
  - `i2s_recoveries=0`;
  - pos-probe: `health=100`, SD montado.

### Playback v2 probe

Status: iniciado em `99a3a17`.

- Probe HTTP explicito:
  - `GET /api/audio/playback-v2`;
  - `POST /api/audio/playback-v2/probe`;
  - `POST /api/audio/playback-v2/stop`.
- Toca chunk sintetico PCM16 curto gerado pelo `audio_playback_service_v2`.
- O v2 nao chama `audio_hal` e nao cria task propria.
- O `audio_service` atual segue como unico escritor do speaker.
- Stop limpa chunks pendentes e `audio_play_stop()` tambem cancela probe v2.
- Sem audio velho depois do cancel.
- Sem afetar captura v1.

Validacao:

- `idf.py build`.
- `bridge/tests`: 160 testes.
- Firmware real em 2026-05-30:
  - probe curto `duration_ms=320`, `amplitude=1200`;
  - `played_chunks=20`, `queued_chunks=0`, `dropped_chunks=0`;
  - cancelamento de probe `1000 ms` por endpoint stop:
    `playing=false`, `queued_chunks=0`, `cancel_count=1`,
    `dropped_chunks=0`;
  - pos-probe: robo em `IDLE`, `health=100`.

### Capture session v2 PCM16

- Captura PCM16 real acompanhada por v2 atras de flag opt-in:
  - `GET /api/audio/capture-v2`;
  - `POST /api/audio/capture-v2/replay`;
  - `POST /api/audio/capture-v2/cancel`.
- O replay sintetico exercita a maquina de estados de
  `voice_capture_session_v2` sem bridge real.
- Replay com `speech_ms=0` termina sem `voice_start_sent` e sem
  `voice_audio_sent`, preservando a regra "wake vazio nao envia STT".
- O componente nao chama `bridge_service` e nao emite `VOICE_START`,
  `AUDIO_CHUNK` ou `VOICE_END`.
- Flag opt-in:
  - chave API/config: `voice_audio_v2_capture_enabled`;
  - chave NVS: `v2cap_en`;
  - default: `0`, preservando v1 como caminho ativo;
  - migracao pontual sem subir `NB_CFG_SCHEMA_VERSION`.
- Hook de roteamento:
  - `audio_service_begin_listen_session()` consulta a flag;
  - flag off: v1 inalterado;
  - flag on: inicia `voice_capture_session_v2_begin_real_pcm16()`;
  - `audio_service` continua enviando PCM16 ao bridge;
  - `voice_capture_session_v2` apenas acompanha estado/contadores v2 e nao
    chama `bridge_service` diretamente.
- Status HTTP:
  - `real_capture_enabled`: flag/config atual;
  - `real_capture`: diferencia replay sintetico (`false`) de sessao PCM16 real
    acompanhada pelo v2 (`true`).
- Server/ops:
  - proxy diagnostico em `/api/device/audio/capture-v2`,
    `/api/device/audio/capture-v2/replay` e
    `/api/device/audio/capture-v2/cancel`;
  - CLI `noisebot_server debug capture-v2` para status/replay/cancel;
  - acao `live` liga `voice_audio_v2_capture_enabled`, aguarda um turno real e
    desliga a flag no final para rollback.
- Cancelamento:
  - `POST /api/audio/capture-v2/cancel` encerra a sessao real via
    `audio_service_end_listen_session(NB_LISTEN_END_CANCELLED)` quando a
    captura v2 e o audio_service estao ativos.
- Validacao:
  - `idf.py build`;
  - testes de contrato de audio v2/audio service.
  - firmware real com `noisebot_server debug capture-v2 live --json`:
    `real_capture_enabled=true`, `real_capture=true`, `state=DONE`,
    `voice_start_sent=true`, `voice_audio_sent=true`,
    `voice_end_sent=true`, `speech_frames=260`,
    `captured_samples=66560`, `dropped_frames=0`,
    `last_error=ESP_OK`, rollback `disabled.ok=true`.
  - regressao `barge-live` com flag v2 desligada:
    `ok=true`, `interrupted_turn_id=85`,
    `interruption_cancel_ms=1.4`, `discard_reason=barge_in`,
    `outcome=interrupted`.
  - regressao `no-echo-live` com flag v2 desligada:
    `ok=true`, `response_turn_id=87`, `unexpected_turn_id=null`,
    `quiet_window_s=10.0`, `outcome=llm`.
  - turno normal com flag v2 desligada:
    `real_capture_enabled=false`, `session_active=false`, novo
    `turn_id=88`, `outcome=local_intent`, `intent_name=local_time`,
    `transcript_quality=good`, sem descarte ou erro.

- Wake abre sessao.
- Silencio apos wake nao envia STT.
- Fala normal gera STT good.
- Barge-in segue ok.
- No-echo segue ok.

### Opus v2

- Inicio seguro da Fase F:
  - `GET /api/audio/codec-v2` expõe o contrato do codec v2;
  - PCM16 segue como formato default;
  - Opus v2 segue opt-in e inativo;
  - contrato publicado: 16 kHz, mono, 60 ms, 960 samples, 32 kbps,
    fila curta de 40 pacotes;
  - nenhum worker/task v2 e criado no boot e o bridge atual nao muda.
  - server proxy: `/api/device/audio/codec-v2`;
  - server proxy: `/api/device/audio/codec-v2/encode-test`;
  - server proxy: `/api/device/audio/codec-v2/drain`;
  - server proxy: `/api/device/audio/codec-v2/reset`;
  - server proxy: `/api/device/audio/codec-v2/opus-encode-test`;
  - server proxy: `/api/device/audio/codec-v2/overflow-test`;
  - server proxy: `/api/device/audio/codec-v2/worker/start`;
  - server proxy: `/api/device/audio/codec-v2/worker/stop`;
  - server proxy: `/api/device/audio/codec-v2/worker/stress-test`;
  - server proxy: `/api/device/audio/codec-v2/worker/feed-test`;
  - CLI: `noisebot_server debug codec-v2 status`;
  - CLI: `noisebot_server debug codec-v2 encode-test`;
  - CLI: `noisebot_server debug codec-v2 drain`;
  - CLI: `noisebot_server debug codec-v2 reset`;
  - CLI: `noisebot_server debug codec-v2 opus-encode-test`;
  - CLI: `noisebot_server debug codec-v2 overflow-test --packets N`;
  - CLI: `noisebot_server debug codec-v2 worker-start`;
  - CLI: `noisebot_server debug codec-v2 worker-stop`;
  - CLI: `noisebot_server debug codec-v2 worker-stress-test --packets N`;
  - CLI: `noisebot_server debug codec-v2 worker-feed-test --frames N`;
  - status atual publica o worker opt-in:
    `worker_supported=true`, `worker_active=false`,
    `worker_state=not_started`, `worker_drained_packets` e contadores
    `worker_opus_*`, sem criar task no boot;
  - `encode-test` e sintetico PCM16 passthrough: incrementa contadores de
    frame/pacote sem worker, sem Opus real, sem bridge e sem captura;
  - packetizer sintetico acumula chunks PCM16 de 256 samples ate frame de
    960 samples; 4 chunks geram 1 pacote e `pending_samples=64`;
  - fila sintetica limitada a 40 pacotes: quando cheia, novo pacote incrementa
    `packet_drops`, sem worker, sem Opus real e sem bridge;
  - drain sintetico limpa apenas a fila pronta e retorna `drained_packets`,
    preservando `pending_samples` e contadores acumulados;
  - reset diagnostico zera `pcm_frames_in`, `packets_out`, `packet_drops`,
    `queue_count` e `pending_samples`, preservando `format=pcm16` e contrato
    fixo do codec;
  - overflow-test diagnostico e autocontido: limpa antes e depois, reporta
    `attempted_packets`, `accepted_packets`, `dropped_packets`,
    `peak_queue_count`, `queue_count_after_cleanup` e
    `status_packet_drops_after_cleanup`, sem poluir status global;
  - `opus-encode-test` e o primeiro Opus real no `audio_codec_service_v2`:
    cria uma task temporaria com stack proprio, abre o encoder Espressif,
    codifica um frame sintetico de 960 samples, fecha o encoder e reporta
    `encoded_bytes`, heap e `codec_error`, sem worker persistente, sem bridge,
    sem captura/playback e sem mudar o PCM16 como padrao;
  - worker opt-in: `worker-start` cria a task FreeRTOS
    `nb_codec_v2_worker` apenas sob comando explicito; ela abre o encoder
    Opus no contexto do worker, consome a fila sintetica, codifica um frame
    sintetico de 960 samples por pacote, incrementa `worker_drained_packets`
    e atualiza `worker_opus_packets`, `worker_opus_encoded_bytes_total` e
    `worker_opus_last_packet_bytes`; nao toca captura, bridge ou playback;
    `worker-stop` solicita parada, drena o restante e retorna
    `worker_state=stopped`;
  - worker-stress-test diagnostico: limpa estado, inicia o worker opt-in,
    enfileira ate 40 pacotes sinteticos completos, espera a task drenar e
    codificar, para o worker e retorna `worker_opus_packets_delta`,
    `worker_opus_encoded_bytes_delta`, `packet_drops_delta`,
    `queue_count_after` e `worker_state_after`, sem captura, bridge ou
    playback;
  - worker-feed-test diagnostico: limpa estado, inicia o worker opt-in,
    alimenta ate 40 frames PCM16 sinteticos de 960 samples pelo
    `audio_codec_service_v2_feed_pcm16()`, espera o packetizer enfileirar,
    a task drenar e codificar, para o worker e retorna
    `pcm_frames_in_delta`, `packets_out_delta`, `worker_opus_packets_delta`,
    `worker_opus_encoded_bytes_delta`, `pending_samples_after`,
    `worker_payload_packets_delta`, `worker_payload_bytes_delta`,
    `worker_payload_last_checksum`, `worker_payload_preview_hex`,
    `packet_drops_delta`, `queue_count_after` e `worker_state_after`, sem
    captura, bridge ou playback;
  - observador de payload do worker Opus: a cada pacote codificado, o worker
    atualiza contadores de payload, bytes totais, sequencia, checksum e preview
    fixa de ate 16 bytes; isso e apenas diagnostico e nao envia dados ao
    bridge;
  - fila egress Opus diagnostica: o worker agora enfileira metadados do pacote
    Opus codificado em uma fila limitada de ate 40 pacotes, com contadores,
    bytes totais, drops, checksum, preview e endpoint
    `/api/audio/codec-v2/egress/drain`; o `worker-feed-test` drena essa fila
    automaticamente ao final e retorna `opus_egress_*`, sem tocar bridge,
    captura ou playback;
  - stub de handoff Opus para bridge: endpoint diagnostico
    `/api/audio/codec-v2/bridge-handoff-test`, proxy server
    `/api/device/audio/codec-v2/bridge-handoff-test` e CLI
    `noisebot_server debug codec-v2 bridge-handoff-test --frames N`; ele roda
    `feed_pcm16 -> worker Opus -> egress`, registra `bridge_handoff_*` como
    pacotes prontos para handoff e retorna `bridge_handoff_stub=true`,
    `bridge_packet_not_sent=true` e `bridge_transport_unchanged=true`; nao
    chama `bridge_service_send_opus_packet()`, nao renegocia HELLO, nao ativa
    `bridge_service_set_opus_enabled()`, nao toca captura/playback e nao
    promove Opus como padrao;
  - controle opt-in de transporte Opus live no namespace Codec v2: endpoints
    `/api/audio/codec-v2/transport/enable` e
    `/api/audio/codec-v2/transport/disable`, proxies server
    `/api/device/audio/codec-v2/transport/enable` e
    `/api/device/audio/codec-v2/transport/disable`, e CLI
    `noisebot_server debug codec-v2 transport-enable|transport-disable --json`;
    o controle agora usa o worker live do proprio `audio_codec_service_v2`.
    `transport-enable` inicia `audio_codec_service_v2_worker_start()`, liga
    `bridge_service_set_opus_enabled(true)` e faz o `audio_service` alimentar
    PCM16 normalizado no codec v2. O worker codifica frames reais de 960
    samples e grava pacotes Opus em uma fila egress real para
    `audio_codec_service_v2_read_opus_packet()` drenar antes de
    `bridge_service_send_opus_packet()`. O JSON retorna
    `transport_worker="audio_codec_service_v2"`,
    `compat_worker="audio_codec_service_v2"` e `pcm16_fallback=true`; PCM16
    continua padrao e rollback imediato via `transport-disable`;
  - reset diagnostico preserva o estado do worker quando ele esta ativo, para
    evitar status incoerente ou uma segunda task acidental;
  - validado em hardware apos flash:
    `initialized=false`, `format=pcm16`, `opus_frame_ms=60`,
    `opus_frame_samples=960`, `opus_bitrate=32000`,
    `max_queue_packets=40`, contadores zerados, `error=ESP_OK`;
    `capture-v2 status` seguiu com `real_capture_enabled=false` e
    `session_active=false`.
  - CLI real com `--host 192.168.1.30` retornou o mesmo contrato.
  - validacao local do `encode-test`: `bridge/tests` 160, `server/tests` 120 e
    `idf.py build`.
  - validacao local do packetizer: teste focado bridge 6, teste focado server
    105, bridge completo 160, server completo 120 e `idf.py build`.
  - validacao local da fila sintetica: teste focado bridge 6, teste focado
    server 105 e `idf.py build`.
  - validacao local do drain sintetico: teste focado bridge 6, teste focado
    server 107, bridge completo 160, server completo 122 e `idf.py build`.
  - validacao local do reset diagnostico: teste focado bridge 6, teste focado
    server 109, bridge completo 160, server completo 124 e `idf.py build`.
  - validacao local do overflow-test diagnostico: teste focado bridge 6, teste
    focado server 111, bridge completo 160, server completo 126 e
    `idf.py build`.
  - validacao local do stub de worker inativo: teste focado bridge 6, teste
    focado server 111, bridge completo 160, server completo 126 e
    `idf.py build`.
  - validacao local do encode Opus real diagnostico: teste focado bridge 6,
    teste focado server 113, bridge completo 160, server completo 128 e
    `idf.py build`.
  - validacao local do worker opt-in: teste focado bridge 6, teste focado
    server 115, bridge completo 160, server completo 130 e `idf.py build`.
  - validacao local do Opus dentro do worker opt-in: teste focado bridge 6,
    teste focado server 115, bridge completo 160, server completo 130 e
    `idf.py build`.
  - validacao local do worker Opus multi-pacote sintetico: teste focado bridge
    6, teste focado server 116 e `idf.py build`.
  - validacao local do caminho feed PCM16 -> worker Opus: teste focado bridge
    6, teste focado server 117 e `idf.py build`.
  - validacao local do observador de payload Opus do worker: teste focado
    bridge 6, teste focado server 117 e `idf.py build`; hardware validado via
    `codec-v2 worker-feed-test --frames 10` com
    `worker_payload_packets_delta=10`, `worker_payload_bytes_delta=2434`,
    preview nao vazio, zero drops e `capture-v2` desligado.
  - validacao local da fila egress Opus diagnostica: teste focado bridge 6,
    teste focado server 119 e `idf.py build`; hardware validado via
    `codec-v2 worker-feed-test --frames 10` com
    `opus_egress_packets_delta=10`, `opus_egress_bytes_delta=2434`,
    `opus_egress_packet_drops_delta=0`,
    `opus_egress_drained_after_test=10`,
    `opus_egress_queue_count_after_cleanup=0`, preview nao vazio,
    status final com `opus_egress_queue_count=0` e `capture-v2` desligado.
  - validacao local e em hardware do stub de handoff Opus para bridge: teste
    focado bridge 6, teste focado server 120 e `idf.py build`; apos flash,
    `codec-v2 bridge-handoff-test --frames 10` retornou
    `bridge_handoff_stub=true`, `bridge_packet_not_sent=true`,
    `bridge_transport_unchanged=true`, `bridge_handoff_packets_ready_delta=10`,
    `bridge_handoff_bytes_ready_delta=2434`,
    `opus_egress_queue_count_after_cleanup=0`, zero drops,
    `worker_state_after=stopped`; status final confirmou `format=pcm16`,
    `bridge_handoff_packets_ready=10`, fila egress zerada, `error=ESP_OK` e
    `capture-v2` desligado.
  - validacao local e em hardware do controle de transporte Opus live pelo
    namespace v2: teste focado bridge 9, teste focado server 121 e
    `idf.py build`; apos flash, `codec-v2 transport-enable --json` retornou
    `ok=true`, `codec_v2_transport=true`, `live_bridge_transport=true`,
    `compat_worker="audio_processor_service"`, `pcm16_fallback=true` e
    `ESP_OK`; o status do worker confirmou task criada, worker rodando,
    Opus 16 kHz/60 ms/32 kbps, `codec_error=0`, stack em PSRAM e
    `last_error=ESP_OK`; `transport-disable --json` retornou
    `live_bridge_transport=false`, `opus_enabled=false` e `ESP_OK`; status
    final confirmou worker parado, `capture-v2` desligado e Codec v2 limpo em
    `format=pcm16`.
  - validacao local da migracao do transporte live para o worker do
    `audio_codec_service_v2`: teste focado bridge 12, server facade 121 e
    `idf.py build` limpo. Validacao em hardware apos flash passou:
    `transport-enable` retornou
    `transport_worker="audio_codec_service_v2"`, `opus_enabled=true` e
    `ESP_OK`; status ativo mostrou worker `running`, `opus_codec_error=0`,
    filas zeradas e zero drops; turno real com transcript `Me conte uma
    história curta.` teve `transcript_quality=good`, `outcome=llm`,
    `turn_id=4`, `chunk_count=39`, `total_samples=37424`,
    `duration_ms=2339.0`, `stt_ms=1088.0`, `first_audio_out_ms=5480.9` e
    resposta LLM falada; worker v2 registrou `worker_opus_packets=39`,
    `worker_opus_encoded_bytes_total=9488`,
    `opus_egress_packets_drained=39`, `packet_drops=0`,
    `opus_egress_packet_drops=0`, `queue_count=0` e `opus_codec_error=0`;
    rollback via `transport-disable` retornou `opus_enabled=false`,
    `ESP_OK`, worker parado e PCM16 como fallback.
  - `codec-ab` curto agora usa o transporte `codec-v2` e foi validado em
    hardware com a frase `me diga uma curiosidade`: PCM16 retornou `ok=true`,
    `turn_id=7`, `outcome=local_intent`, transcript `Diga uma curiosidade.`,
    `transcript_similarity=0.858`, `duration_ms=3200.0`, `stt_ms=1060.0`;
    Opus v2 retornou `ok=true`, `turn_id=8`, `outcome=local_intent`,
    transcript `Me diga uma curiosidade.`, `transcript_similarity=1.0`,
    `duration_ms=5219.0`, `stt_ms=1107.8`, `packets_drained=87`,
    `packet_drops=0`, `encoded_bytes=21368` e
    `server_codec_confirmed=true`.
  - observacao de limpeza: apos o A/B curto, ficou 1 pacote egress pendente
    apos rollback; foi drenado por `codec-v2 egress-drain`. Correcao local:
    `transport-disable` passa a drenar egress e retornar
    `egress_drained_packets`; o harness `codec-ab` tambem chama
    `egress-drain` apos desabilitar Opus.
  - validacao em hardware apos flash da correcao de rollback:
    `codec-ab --repeat 3 "me diga uma curiosidade"` passou com 3/3 PCM16 e
    3/3 Opus v2 `ok=true`; PCM16 teve STT medio ~1081.0 ms, Opus v2 teve STT
    medio ~1086.6 ms, todos os turnos Opus transcreveram `Me diga uma
    curiosidade.`, `transcript_similarity=1.0`, `packet_drops=0`,
    334 pacotes drenados pelo harness e 81213 bytes Opus; status final do
    Codec v2 confirmou `worker_state=stopped`, `opus_egress_queue_count=0`,
    `opus_egress_packet_drops=0`, `opus_codec_error=0` e `capture-v2`
    desligado.
  - preparacao local para regressao de turn-taking com Opus v2:
    `barge-live` e `no-echo-live` agora aceitam `--codec opus-v2`; os harnesses
    ligam `codec-v2 transport-enable`, medem pacotes/drops/bytes, e sempre
    fazem rollback com `transport-disable` + `egress-drain`. `server/tests`
    completo passou com 138 testes.
  - validacao em hardware de turn-taking com Opus v2 opt-in:
    `barge-live --codec opus-v2` retornou `ok=true`,
    `interruption_cancel_ms=1.6`, `discard_reason=barge_in`, 137 pacotes Opus,
    33558 bytes e `packet_drops=0`; `no-echo-live --codec opus-v2` retornou
    `ok=true`, `unexpected_turn_id=null` em 10s de silencio, 56 pacotes Opus,
    13856 bytes e `packet_drops=0`.
  - promocao local de capability Opus v2 opt-in no contrato HELLO/status:
    `codecs` passa a significar apenas transporte ativo e continua
    `pcm16=true`, `opus=false` no HELLO padrao; `codec_options` anuncia
    `opus_tx=true`, `opus_default=false`, 16 kHz mono, 60 ms/960 samples e
    32 kbps. O HELLO Opus ativo preserva `codecs.opus=true` somente apos enable
    explicito. O server espelha `codec_options` em `/api/ai/status`.
    Validacao local: contratos bridge focados passaram, `bridge/tests` completo
    passou com 160 testes, `server/tests` completo passou com 138 testes e
    `idf.py build` passou.
  - validacao em hardware da capability Opus v2 opt-in apos flash:
    server reiniciado com `qwen3.5:9b`; `/ai/status` confirmou
    `audio.format=pcm16`, `codecs.pcm16=true`, `codecs.opus=false`,
    `codec_options.opus_tx=true` e `opus_default=false`; `codec-v2
    transport-enable` alternou o contrato ativo para Opus, `transport-disable`
    voltou para PCM16, e o status final confirmou `worker_state=stopped`,
    `opus_egress_queue_count=0`, zero drops, `opus_codec_error=0` e
    `capture-v2` desligado.
  - promocao configuravel no server: `NOISEBOT_AUDIO_DEFAULT_CODEC` e
    `--audio-codec` aceitam `pcm16|opus-v2`; o default de fabrica continua
    `pcm16`, mas `opus-v2` chama `codec-v2 transport-enable` no startup do
    server. Validacao local: `server/tests` passou com 142 testes. Validacao
    live: server subiu com `--audio-codec opus-v2`, `/ai/status` confirmou
    `audio.format=opus`, `codecs.opus=true`, worker v2 `running` e
    `opus_codec_error=0`; rollback via `transport-disable` e restart normal
    voltou para `audio.format=pcm16`, worker `stopped`, fila zero e
    `capture-v2` desligado.
  - soak real com startup Opus v2: usuario fez varios testes com intents locais
    e LLM. Status pos-uso confirmou server conectado, Opus ativo, ultimo turno
    `outcome=llm` sem erro, `pcm_frames_in=579`, `packets_out=579`,
    `worker_opus_packets=579`, `opus_egress_packets_drained=579`,
    `worker_opus_encoded_bytes_total=140756`, zero drops, egress queue zero,
    worker `running`, `opus_codec_error=0` e `capture-v2` desligado.
  - promocao local aplicada: `server/.env` define
    `NOISEBOT_AUDIO_DEFAULT_CODEC=opus-v2`; reinicio do server sem
    `--audio-codec` confirmou `/ai/status` em Opus ativo com `qwen3.5:9b`,
    worker v2 `running`, `packets_out=738`, `opus_egress_packets_drained=738`,
    zero drops, fila egress zero, `opus_codec_error=0` e `capture-v2`
    desligado. PCM16 permanece rollback por env `pcm16` ou
    `codec-v2 transport-disable`.
  - diagnostico de corte de resposta adicionado no server: cada sessao recente
    passa a expor contadores de completude TTS/playback (`tts_chunks_sent`,
    `tts_pcm_bytes_in`, `tts_pcm_bytes_sent`, `tts_padding_bytes`,
    `tts_say_begin_sent`, `tts_say_end_sent`, `tts_expected_duration_ms`,
    `tts_completed`) e campos de limite visual (`text_scroll_bytes`,
    `text_scroll_payload_bytes`, `text_scroll_truncated`). Objetivo: separar
    corte de audio, corte visual de `TEXT_SCROLL` de 128 bytes e falha de envio
    SAY sem mexer em firmware/protocolo. Validacao local: `server/tests` passou
    com 143 testes.
  - analise automatica do corte de resposta em `/ai/metrics`: quando
    `tts_completed=false`, `voice_alert` retorna "Fala possivelmente
    incompleta" e recomenda checar chunks SAY, `SAY_BEGIN/SAY_END` e
    cancelamentos; quando `text_scroll_truncated=true` mas TTS completou,
    `voice_diagnosis` identifica limite visual de `TEXT_SCROLL`, sem marcar
    alerta de audio. Validacao local: `server/tests` passou com 145 testes.
  - validacao real de resposta longa apos a instrumentacao: `/ai/metrics`
    mostrou transcript `Me conte uma história longa.`, `tts_completed=true`,
    `tts_say_end_sent=true`, 589 chunks SAY, `tts_expected_duration_ms=9424.0`,
    `voice_alert=null` e diagnostico apenas de truncamento visual
    `TEXT_SCROLL`. O `last_voice_session.reply` agora preserva ate 1200 chars
    para diagnostico, evitando falso corte no proprio endpoint de metricas.
    Validacao local: `server/tests` passou com 146 testes.
  - paginacao visual server-side para respostas longas: o server divide a
    resposta em paginas UTF-8 seguras de ate 128 bytes e envia multiplos
    `TEXT_SCROLL` espacados durante a fala. Nao cria novo opcode, nao exige
    firmware novo e nao altera audio/Opus/PCM16. `/ai/metrics` passa a expor
    `text_scroll_pages` e `text_scroll_pages_sent`; diagnostico reconhece
    paginacao concluida. Validacao local: `server/tests` passou com 149 testes.
  - refino de largura visual do `TEXT_SCROLL`: uma frase media pode caber em
    128 bytes mas ainda depender do scroll horizontal lento do overlay do
    firmware. O server agora limita paginas tambem a aproximadamente 38
    caracteres, mantendo UTF-8 seguro e sem novo opcode. Isso e server-only,
    nao altera TTS, Opus, PCM16, captura nem playback. Validacao local: teste
    focado de paginacao e `server/tests` passaram com 150 testes.
  - guardrail operacional do Codec v2: o server agora oferece `codec-v2 health`
    no CLI e `/api/device/audio/codec-v2/health` no proxy. Ele le o status do
    firmware e classifica `ok`, `warn` ou `degraded` por drops,
    `opus_codec_error`, worker, fila pronta e fila egress, retornando
    `issues`, `warnings` e hint de rollback. Nao muda firmware/protocolo nem
    audio. Validacao local: teste focado de health/CLI e `server/tests`
    passaram com 154 testes. Validacao live: detectou `opus_egress_queue_count=1`,
    `egress-drain` drenou 1 pacote, e o health voltou `status=ok`,
    `healthy=true`, zero drops e `opus_codec_error=0`.
  - fechamento da migracao Opus v2: default local do server confirmado com
    `NOISEBOT_AUDIO_DEFAULT_CODEC=opus-v2`; `/ai/status` confirmou server
    conectado com `qwen3.5:9b` e audio ativo em Opus 16 kHz mono, 60 ms;
    `codec-v2 health` retornou `healthy=true`, `status=ok`, sem
    issues/warnings, zero drops, fila egress zero, `opus_codec_error=0` e
    worker `running`. Rollback PCM16 segue documentado por env `pcm16`,
    reinicio do server ou `codec-v2 transport-disable`.
  - validacao em hardware de turno Opus live curto pelo namespace Codec v2 com
    server em `NOISEBOT_LLM_MODEL=qwen3.5:9b`: transcript `Fale uma frase
    curta.`, `transcript_quality=good`, `outcome=llm`, reply `Ola! Sou o
    NoiseBot e estou ansioso para conversar com voce.`, `total_samples=51824`,
    `duration_ms=3239.0`, `stt_ms=1094.3`, `first_audio_out_ms=5490.9`,
    `tts_first_audio_ms=471.1`; worker de compatibilidade registrou
    `pcm_encode_packets=54`, `opus_packet_enqueued=54`,
    `opus_packet_drained=54`, `opus_packet_drops=0`,
    `opus_packet_queue_count=0`, `opus_packet_bytes_total=13110` e
    `codec_error=0`; rollback desligou Opus e status final confirmou
    `capture-v2` desligado e Codec v2 limpo em `format=pcm16`.
  - validacao em hardware do caminho feed PCM16 -> worker Opus:
    `worker-feed-test --frames 10` retornou `attempted_frames=10`,
    `attempted_samples=9600`, `pcm_frames_in_delta=10`,
    `packets_out_delta=10`, `worker_drained_packets_delta=10`,
    `worker_opus_packets_delta=10`, `worker_opus_encoded_bytes_delta=2434`,
    `worker_opus_last_packet_bytes=242`, `packet_drops_delta=0`,
    `queue_count_after=0`, `pending_samples_after=0`,
    `worker_state_after=stopped`, `error=ESP_OK`; `capture-v2 status` apos o
    teste permaneceu desligado e em `IDLE_SESSION`.
  - validacao em hardware do worker Opus multi-pacote sintetico:
    `worker-stress-test --packets 10` retornou `accepted_packets=10`,
    `worker_drained_packets_delta=10`, `worker_opus_packets_delta=10`,
    `worker_opus_encoded_bytes_delta=2434`,
    `worker_opus_last_packet_bytes=242`, `packet_drops_delta=0`,
    `queue_count_after=0`, `worker_state_after=stopped`, `error=ESP_OK`;
    `capture-v2 status` apos o teste permaneceu desligado e em
    `IDLE_SESSION`.
  - observacao de hardware do Opus dentro do worker opt-in: a primeira
    tentativa com stack persistente de 24 KB falhou em `worker-start` com HTTP
    409 e `worker_state=error`, sem derrubar HTTP nem tocar captura; a
    tentativa com stack interna de 12 KB criou a task, mas o encode deixou HTTP
    indisponivel. A correcao local voltou o worker para 24 KB com stack em
    PSRAM via `xTaskCreatePinnedToCoreWithCaps` e delecao por
    `vTaskDeleteWithCaps`; teste focado/build passaram. A validacao em hardware
    apos flash passou: `worker-start` retornou `ok=true`; `encode-test` com
    worker ativo foi drenado pela task; status confirmou `queue_count=0`,
    `worker_drained_packets=1`, `worker_opus_packets=1`,
    `worker_opus_encoded_bytes_total=248`,
    `worker_opus_last_packet_bytes=248`, `packet_drops=0`, `error=ESP_OK`;
    `worker-stop` deixou `worker_active=false`, `worker_state=stopped`; e
    `capture-v2 status` permaneceu desligado.
  - observacao de hardware: a tentativa inicial de rodar o encode Opus
    sincronamente no handler HTTP causou timeout e indisponibilidade HTTP; a
    correcao moveu o encode para task temporaria com stack proprio.
  - validacao em hardware do stub de worker inativo apos flash:
    `codec-v2 status` retornou `worker_supported=false`,
    `worker_active=false`, `worker_state=not_started`, contadores zerados,
    `format=pcm16` e `error=ESP_OK`; `capture-v2 status` seguiu com
    `real_capture_enabled=false`, `session_active=false`,
    `state=IDLE_SESSION` e `last_error=ESP_OK`.
  - validacao em hardware do overflow-test diagnostico apos flash:
    `packets=40` resultou em `accepted_packets=40`, `dropped_packets=0`;
    `packets=41` resultou em `accepted_packets=40`, `dropped_packets=1`;
    `packets=45` resultou em `accepted_packets=40`, `dropped_packets=5`;
    todos com `peak_queue_count=40`, `queue_count_after_cleanup=0`,
    `status_packet_drops_after_cleanup=0`, `error=ESP_OK`; status final do
    codec voltou zerado e `capture-v2 status` seguiu com
    `real_capture_enabled=false`, `session_active=false`, `last_error=ESP_OK`.
  - validacao em hardware do reset diagnostico apos flash:
    status inicial zerado, `encode-test` gerou `queue_count=1` e
    `pending_samples=64`, `reset` zerou contadores/fila/pendencias,
    preservou `format=pcm16` e `error=ESP_OK`; `capture-v2 status` seguiu
    com `real_capture_enabled=false`, `session_active=false`,
    `last_error=ESP_OK`.
  - validacao em hardware do drain sintetico apos flash:
    status inicial zerado, `encode-test` gerou `queue_count=1` e
    `pending_samples=64`, `drain` retornou `drained_packets=1` e zerou
    `queue_count`, status final preservou `pending_samples=64` e
    `capture-v2 status` seguiu com `real_capture_enabled=false`,
    `session_active=false`, `last_error=ESP_OK`.
  - validacao em hardware do `encode-test` apos flash:
    `pcm_frames_in=1`, `packets_out=1`, `packet_drops=0`,
    `queue_count=0`, `pending_samples=64`, `error=ESP_OK`.
  - validacao em hardware do worker opt-in apos flash:
    status inicial `worker_supported=true`, `worker_active=false`,
    `worker_state=not_started`; `worker-start` retornou `ok=true`;
    `encode-test` com worker ativo gerou `queue_count=1`; status seguinte
    confirmou `queue_count=0`, `worker_drained_packets=1`, `packet_drops=0`;
    `worker-stop` retornou `worker_active=false`, `worker_state=stopped`;
    `capture-v2 status` permaneceu com `real_capture_enabled=false`,
    `session_active=false`, `state=IDLE_SESSION`, `last_error=ESP_OK`.
  - validacao em hardware do `opus-encode-test` corrigido apos flash:
    retornou `ok=true`, `test_format=opus`, `frame_samples=960`,
    `encoded_bytes=248`, `codec_error=0`, `opus_encode_tests=1`,
    `opus_last_packet_bytes=248`, `queue_count=0`, `packet_drops=0`,
    `worker_active=false`, `worker_state=stopped`, `error=ESP_OK`;
    status seguinte confirmou HTTP saudavel, fila zerada e
    `opus_codec_error=0`; `capture-v2 status` permaneceu desligado.
  - validacao em hardware do Opus no worker opt-in com stack PSRAM:
    `codec-v2 status` iniciou limpo com `worker_supported=true`,
    `worker_active=false`, `worker_state=not_started`; `worker-start`
    retornou `ok=true`; `encode-test` com worker ativo gerou
    `packets_out=1`, `queue_count=1` e `opus_codec_error=0`; status seguinte
    confirmou `queue_count=0`, `worker_drained_packets=1`,
    `worker_opus_packets=1`, `worker_opus_encoded_bytes_total=248`,
    `worker_opus_last_packet_bytes=248`, `packet_drops=0`, `error=ESP_OK`;
    `worker-stop` retornou `worker_active=false`, `worker_state=stopped`; e
    `capture-v2 status` permaneceu com `real_capture_enabled=false`,
    `session_active=false`, `state=IDLE_SESSION`, `last_error=ESP_OK`.
- Packet drops zero em teste curto.
- Transcript comparavel ao PCM16.
- `server_codec_confirmed=true`.
- PCM16 rollback intacto.

## Perguntas Para Uma IA Antes de Mexer

Antes de qualquer alteracao, a IA deve responder:

1. Qual componente estou alterando?
2. Esse componente pertence a captura, playback, VAD, codec, bridge ou policy?
3. Estou mexendo em algo que ja funciona?
4. O pipeline PCM16 continua como fallback?
5. Wake word sera afetada?
6. Barge-in sera afetado?
7. Existe teste automatico ou harness manual para validar?
8. Qual e o rollback?
9. Essa mudanca copia um principio do NoiseBot ou so adiciona remendo?
10. O hardware NoiseBot suporta a feature ou estou assumindo um recurso que a
    placa atual nao possui?

Se alguma resposta for incerta, pare e investigue.

## Prompt Sugerido Para Consultar Esta Nota

Use este prompt quando outra IA for ajudar:

```text
Leia docs/OBSIDIAN_VOICE_AUDIO_V2_KNOWLEDGE.md e
docs/VOICE_AUDIO_V2_ARCHITECTURE.md antes de sugerir qualquer mudanca.
Nao altere wake word, VAD thresholds, state machine, barge-in ou follow-up sem
justificativa direta. Preserve PCM16 como fallback. O objetivo atual e construir
audio v2 paralelo, por fases, usando NoiseBot como referencia de
arquitetura, nao como copia cega de hardware.
```

## Glossario Rapido

- AEC: Acoustic Echo Cancellation. Remove do mic o audio que saiu no speaker.
  No NoiseBot atual nao ha referencia limpa de speaker.
- AFE: Audio Front-End da Espressif. Pode fazer VAD/NS/AEC dependendo da config.
- Barge-in: interromper a fala do robo para falar novo comando.
- DTX: Discontinuous Transmission no Opus, reduz envio em silencio.
- FEC: Forward Error Correction no Opus, util em rede com perdas; desligado no
  perfil atual.
- NS: Noise Suppression.
- PCM16: audio cru 16-bit signed.
- Pre-roll: audio guardado antes do inicio oficial da captura para nao perder a
  primeira silaba.
- SAY: chunk de audio vindo do bridge para o robo falar.
- VAD: Voice Activity Detection.

## Links Internos

- [[VOICE_AUDIO_V2_ARCHITECTURE]]
- [[VOICE_AUDIO_V2_NEXT_PHASES]]
- [[VOICE_AUDIO_V2_RELEASE_CHECKLIST]]
- [[VOICE_PIPELINE]]
- [[VOICE_OPUS_QUALITY]]
- [[VOICE_AB_PHASE5]]
- [[VOICE_AB_PHASE5_8192]]
- [[VOICE_SAMPLES_PHASE4]]
- [[REFERENCE_ARCHITECTURES]]
- [[ROADMAP]]
- [[ARCHITECTURE]]
- [[HARDWARE]]
