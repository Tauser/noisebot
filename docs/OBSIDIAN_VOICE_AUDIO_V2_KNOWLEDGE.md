---
title: NoiseBot Voice Audio v2 Knowledge Base
created: 2026-05-30
status: active-reference
project: NoiseBot
tags:
  - noisebot
  - voice
  - audio-v2
  - xiaozhi
  - stackchan
  - esp32s3
  - opus
  - vad
  - aec
  - firmware
---

# NoiseBot Voice Audio v2 Knowledge Base

Esta nota e feita para Obsidian e para consulta por IAs futuras. Ela resume o
conhecimento tecnico consolidado sobre a migracao de voz do NoiseBot, com base
no que foi levantado em Xiaozhi, StackChan e no proprio firmware atual.

Use esta nota como entrada rapida antes de alterar qualquer parte de audio,
captura, reproducao, VAD, Opus, bridge, wake word ou barge-in.

## Leitura Obrigatoria

- [[VOICE_AUDIO_V2_ARCHITECTURE]]
- [[VOICE_PIPELINE]]
- [[ROADMAP]]
- [[REFERENCE_ARCHITECTURES]]
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

Referencias externas locais:

- `D:\Projetos\Xiaozhi-for-XiaoESP32S3-master\Source\xiaozhi-esp32-2.2.2`
- `D:\Projetos\StackChan`

## Resumo Executivo

O NoiseBot precisa refazer o subsistema de voz em uma arquitetura v2 paralela,
sem quebrar o pipeline atual. O problema principal nao e apenas Opus. O problema
e que captura, playback, VAD, pre-roll, bridge, Opus, AFE shadow, diagnostico e
recuperacao I2S estao concentrados demais no `audio_service.c`.

Meta da arquitetura v2:

- Separar Audio I/O, playback, voice activity, capture session, codec e bridge.
- Preservar PCM16 como fallback padrao.
- Manter Opus como opt-in ate validacao maior.
- Manter wake word e barge-in atuais intactos enquanto a base v2 nasce.
- Usar Xiaozhi/StackChan como referencia de arquitetura, nao como copia cega.

Regra principal:

> Nao consertar wake, VAD, Opus, AEC e playback ao mesmo tempo.

## Decisoes Fixadas

### PCM16

- Continua sendo o caminho padrao.
- Continua sendo fallback obrigatorio.
- Nao remover ate audio v2 provar estabilidade em hardware real.

### Opus

- Continua opt-in por API.
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
- 60 ms vem de Xiaozhi/StackChan.
- 32 kbps vem do diagnostico offline nos WAVs reais do NoiseBot.

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

- StackChan/CoreS3 tem `AUDIO_INPUT_REFERENCE=true` e codec ES7210/AW88298.
- NoiseBot atual nao tem canal limpo de referencia de speaker.
- AEC device-side nao deve ser promovido no hardware atual.
- AFE pode ser usado para VAD/NS sem prometer AEC.
- Server-side AEC so depois de timestamps de playback e desenho explicito.

## O Que Aprendemos do Xiaozhi

Fontes principais:

- `main/audio/audio_service.h`
- `main/audio/audio_service.cc`
- `main/audio/processors/afe_audio_processor.cc`
- `main/application.cc`
- `main/protocols/websocket_protocol.cc`
- `docs/websocket.md`

Arquitetura do Xiaozhi:

```text
MIC -> Processor -> Encode Queue -> Opus Encoder -> Send Queue -> Server
Server -> Decode Queue -> Opus Decoder -> Playback Queue -> Speaker
```

Pontos importantes:

- Audio input task separada.
- Audio output task separada.
- Opus codec task separada.
- Filas curtas.
- Audio processor plugavel.
- Wake word plugavel.
- Protocolo anuncia `audio_params`.
- Estados de conversa sao explicitos.

Parametros Xiaozhi relevantes:

```text
OPUS_FRAME_DURATION_MS = 60
MAX_DECODE_PACKETS_IN_QUEUE = 2400 / 60 = 40
MAX_SEND_PACKETS_IN_QUEUE = 2400 / 60 = 40
Opus sample rate = 16000
Opus channels = 1
Opus complexity = 0
Opus FEC = false
Opus DTX = true
Opus VBR = true
```

Tasks Xiaozhi relevantes:

```text
audio_input  priority 8
audio_output priority 4
opus_codec   priority 2, stack 2048 * 12
audio_communication AFE priority 3, stack 4096
```

Protocolo Xiaozhi:

```json
{
  "type": "hello",
  "version": 1,
  "audio_params": {
    "format": "opus",
    "sample_rate": 16000,
    "channels": 1,
    "frame_duration": 60
  }
}
```

Fluxo de estado Xiaozhi:

```text
Idle -> Connecting -> Listening -> Speaking -> Listening/Idle
Listening/Speaking -> abort/close -> Idle
```

Wake durante fala:

- Detectou wake durante speaking.
- Aborta speaking.
- Reabre listening.

## O Que Aprendemos do StackChan

StackChan reaproveita o core Xiaozhi. Ele nao e um projeto totalmente separado
de voz. Isso importa: a licao e copiar a arquitetura validada, nao inventar uma
segunda stack de audio.

Fontes principais:

- `firmware/main/CMakeLists.txt`
- `firmware/main/hal/board/config.h`
- `firmware/main/hal/board/cores3_audio_codec.cc`
- `firmware/main/hal/board/stackchan.cc`

Hardware StackChan/CoreS3:

```text
AUDIO_INPUT_REFERENCE = true
AUDIO_INPUT_SAMPLE_RATE = 24000
AUDIO_OUTPUT_SAMPLE_RATE = 24000
Input codec = ES7210
Output codec = AW88298
```

Consequencia:

- StackChan consegue ter referencia de playback no input.
- Isso viabiliza AEC device-side de forma mais realista.
- NoiseBot com INMP441 + MAX98357A nao tem essa referencia limpa hoje.

## O Que Nao Copiar

Nao copiar diretamente:

- ES7210/AW88298.
- `AUDIO_INPUT_REFERENCE=true`.
- AEC device-side como se fosse universal.
- WebSocket/MQTT inteiro do Xiaozhi.
- C++/`std::vector`/`std::deque` para o firmware C17.
- Reamostragem 24 kHz do CoreS3 como requisito imediato.

Copiar como principio:

- Separacao de responsabilidades.
- Filas curtas.
- Codec task dedicada.
- Audio input/output separados.
- `hello` com capabilities claras.
- Estados de conversa explicitos.
- Abort/cancel limpo.
- AFE como modulo plugavel.

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
- volume;
- cancel/stop;
- descarte de fila velha.

Nao faz:

- captura;
- VAD;
- STT;
- wake.

#### `voice_activity_service_v2`

Faz:

- ESP-SR VAD primario;
- RMS/ZCR/espectral como telemetria;
- AFE/NS opcional;
- eventos internos speech/silence.

Nao faz:

- abrir sessao em IDLE;
- mandar bridge;
- decidir wake.

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
| AEC probe falha | sem referencia/heap | processor status | forcar AEC device-side |
| Crash I2S/ISR | I/O/recovery | audio_hal, task stack, DMA | adicionar processamento no ISR |
| STT audio_curto | VOICE_START/END errado | session state, total_samples | culpar LLM |

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
```

No echo:

```powershell
python -m noisebot_server --host 192.168.1.30 debug no-echo-live "me conte uma historia longa" --json
```

Capture v2 status/replay:

```powershell
python -m noisebot_server --host 192.168.1.30 debug capture-v2 status --json
python -m noisebot_server --host 192.168.1.30 debug capture-v2 replay --speech-ms 640 --silence-ms 900 --json
python -m noisebot_server --host 192.168.1.30 debug capture-v2 live --json
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
  - nenhum worker/task v2 e criado e o bridge atual nao muda.
  - server proxy: `/api/device/audio/codec-v2`;
  - server proxy: `/api/device/audio/codec-v2/encode-test`;
  - server proxy: `/api/device/audio/codec-v2/drain`;
  - server proxy: `/api/device/audio/codec-v2/reset`;
  - server proxy: `/api/device/audio/codec-v2/overflow-test`;
  - CLI: `noisebot_server debug codec-v2 status`;
  - CLI: `noisebot_server debug codec-v2 encode-test`;
  - CLI: `noisebot_server debug codec-v2 drain`;
  - CLI: `noisebot_server debug codec-v2 reset`;
  - CLI: `noisebot_server debug codec-v2 overflow-test --packets N`;
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
9. Essa mudanca copia um principio do Xiaozhi/StackChan ou so adiciona remendo?
10. O hardware NoiseBot suporta a feature ou estou assumindo recurso do CoreS3?

Se alguma resposta for incerta, pare e investigue.

## Prompt Sugerido Para Consultar Esta Nota

Use este prompt quando outra IA for ajudar:

```text
Leia docs/OBSIDIAN_VOICE_AUDIO_V2_KNOWLEDGE.md e
docs/VOICE_AUDIO_V2_ARCHITECTURE.md antes de sugerir qualquer mudanca.
Nao altere wake word, VAD thresholds, state machine, barge-in ou follow-up sem
justificativa direta. Preserve PCM16 como fallback. O objetivo atual e construir
audio v2 paralelo, por fases, usando Xiaozhi/StackChan como referencia de
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
- [[VOICE_PIPELINE]]
- [[VOICE_OPUS_QUALITY]]
- [[VOICE_AB_PHASE5]]
- [[VOICE_AB_PHASE5_8192]]
- [[VOICE_SAMPLES_PHASE4]]
- [[REFERENCE_ARCHITECTURES]]
- [[ROADMAP]]
- [[ARCHITECTURE]]
- [[HARDWARE]]
