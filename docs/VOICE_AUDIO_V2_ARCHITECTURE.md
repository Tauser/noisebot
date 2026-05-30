# Voice Audio v2 Architecture

Data: 2026-05-30
Status: proposta tecnica para refactor paralelo, sem substituir o pipeline atual.
Branch de trabalho: `voice-reference-architecture`.

Este documento fixa o mapa de referencia para refazer o subsistema de voz do
NoiseBot usando Xiaozhi/StackChan como base tecnica, mas sem perder o que ja
funciona no hardware atual.

Versao para consulta rapida em Obsidian/IA:
`docs/OBSIDIAN_VOICE_AUDIO_V2_KNOWLEDGE.md`.

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

## Fontes de Referencia

### Xiaozhi

Arquivos analisados:

- `D:\Projetos\Xiaozhi-for-XiaoESP32S3-master\Source\xiaozhi-esp32-2.2.2\main\audio\audio_service.h`
- `D:\Projetos\Xiaozhi-for-XiaoESP32S3-master\Source\xiaozhi-esp32-2.2.2\main\audio\audio_service.cc`
- `D:\Projetos\Xiaozhi-for-XiaoESP32S3-master\Source\xiaozhi-esp32-2.2.2\main\audio\processors\afe_audio_processor.cc`
- `D:\Projetos\Xiaozhi-for-XiaoESP32S3-master\Source\xiaozhi-esp32-2.2.2\main\application.cc`
- `D:\Projetos\Xiaozhi-for-XiaoESP32S3-master\Source\xiaozhi-esp32-2.2.2\main\protocols\websocket_protocol.cc`
- `D:\Projetos\Xiaozhi-for-XiaoESP32S3-master\Source\xiaozhi-esp32-2.2.2\docs\websocket.md`

Pontos tecnicos absorvidos:

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

### StackChan

Arquivos analisados:

- `D:\Projetos\StackChan\firmware\main\CMakeLists.txt`
- `D:\Projetos\StackChan\firmware\main\hal\board\config.h`
- `D:\Projetos\StackChan\firmware\main\hal\board\stackchan.cc`
- `D:\Projetos\StackChan\firmware\main\hal\board\cores3_audio_codec.cc`
- `D:\Projetos\StackChan\firmware\main\hal\board\cores3_audio_codec.h`

Pontos tecnicos absorvidos:

- StackChan reaproveita o core Xiaozhi, nao reimplementa tudo do zero.
- CoreS3 usa codec ES7210 para microfone e AW88298 para speaker.
- `AUDIO_INPUT_REFERENCE = true` no CoreS3.
- Input e output do CoreS3 rodam a 24 kHz no board analisado.
- O codec CoreS3 abre entrada com canal de referencia quando disponivel:
  `input_channels = input_reference ? 2 : 1`.
- O caminho de AEC do StackChan depende dessa referencia de playback no input.

## Divergencias do NoiseBot

Hardware atual:

- ESP32-S3 N16R8.
- INMP441 como microfone digital I2S.
- MAX98357A como speaker I2S.
- Sem codec ES7210.
- Sem AW88298.
- Sem canal limpo de referencia de speaker no input.
- I2S atual em 16 kHz, mono logico, com fio 32-bit/stereo no HAL.

Consequencias:

- Podemos copiar a arquitetura Xiaozhi/StackChan.
- Nao podemos copiar diretamente o AEC device-side do CoreS3.
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

- 60 ms e filas curtas seguem Xiaozhi/StackChan.
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

## Mapeamento Referencia -> NoiseBot

| Tema | Xiaozhi/StackChan | NoiseBot v2 |
| --- | --- | --- |
| Codec upstream | Opus 16 kHz mono 60 ms | Opus opt-in 16 kHz mono 60 ms 32 kbps |
| Fallback | Nativo no ecossistema Xiaozhi | PCM16 mantido como padrao seguro |
| Input task | Task dedicada `audio_input` | `audio_io_service_v2` input path |
| Output task | Task dedicada `audio_output` | `audio_playback_service_v2`/output path |
| Codec task | Task `opus_codec`, stack grande, prioridade baixa | `audio_codec_service_v2`, worker dedicado |
| Processor | AFE/NoAudioProcessor plugavel | `voice_activity_service_v2` + AFE opcional |
| Wake | AFE/ESP wake word plugavel | preservar `wake_service` atual |
| Barge-in | wake durante speaking aborta fala | preservar caminho atual por wake word |
| AEC device | depende de input reference | nao promover sem referencia |
| AEC server | timestamps no protocolo binario v2 | futuro, se criarmos timestamps/playback ref |
| Downlink | Opus decode + resample se necessario | manter SAY PCM atual; Opus downlink e fase futura |
| Estado | Idle/Connecting/Listening/Speaking | mapear para IDLE/ATTENTIVE/RESPONDING sem reescrever state machine |

## O Que Nao Podemos Copiar Diretamente

- ES7210/AW88298: o NoiseBot usa INMP441/MAX98357A.
- `AUDIO_INPUT_REFERENCE=true`: nao existe canal de referencia limpo no hardware
  atual.
- AEC device-side: sem referencia, vira consumo de memoria sem garantia.
- Input 24 kHz do CoreS3: nosso caminho atual e 16 kHz; STT e Opus upstream ja
  estao em 16 kHz.
- WebSocket/MQTT Xiaozhi completo: nosso bridge TCP local ja funciona e possui
  testes. Copiar transporte inteiro aumentaria risco.
- C++/std::vector/deque do Xiaozhi: o firmware NoiseBot e C17; usar filas
  FreeRTOS e buffers estaticos/PSRAM.

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
- encoder worker dedicado em etapa posterior;
- fila curta em etapa posterior;
- HELLO/capabilities coerentes.

Implementacao atual:

- `audio_codec_service_v2` continua inativo e nao e inicializado no boot;
- `GET /api/audio/codec-v2` retorna constantes do contrato e contadores
  zerados do skeleton;
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
- o server expoe proxy diagnostico em `/api/device/audio/codec-v2`;
- o server tambem expoe `/api/device/audio/codec-v2/encode-test`;
- o server tambem expoe `/api/device/audio/codec-v2/drain`;
- CLI `noisebot_server debug codec-v2 status` consulta o endpoint do firmware;
- CLI `noisebot_server debug codec-v2 encode-test` aciona o teste sintetico;
- CLI `noisebot_server debug codec-v2 drain` drena a fila sintetica;
- o endpoint nao liga Opus, nao cria task e nao altera bridge/captura/playback;
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
- Validacao em hardware do `encode-test` apos flash:
  - `noisebot_server --host 192.168.1.30 debug codec-v2 encode-test --json`;
  - `ok=true`, `initialized=false`, `format=pcm16`,
    `pcm_frames_in=1`, `packets_out=1`, `packet_drops=0`,
    `queue_count=0`, `pending_samples=64`, `error=ESP_OK`.

Aceite:

- `opus-live` ok.
- `codec-ab` curto ok.
- packet drops = 0.
- PCM16 fallback intacto.

### Fase G - AFE/VAD/NS Opcional

Objetivo: aproximar do Xiaozhi sem prometer AEC.

Entregas:

- AFE para VAD/NS em modo opt-in;
- sem AEC device se `input_reference=false`;
- metricas comparando VAD ESP-SR vs AFE.

Aceite:

- Reduz falso start sem matar fala normal.
- Nao altera wake word.
- Nao promove AEC sem criterio.

### Fase H - Decisao de Promocao

Objetivo: decidir se v2 vira padrao.

Aceite minimo:

- PCM16 v2 >= PCM16 v1 em STT e estabilidade.
- Opus v2 sem perda semantica relevante no ambiente real.
- Barge-in por wake segue ok.
- No-echo segue ok.
- Sem aumento perigoso de heap/CPU.
- Rollback documentado.

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

- PCM16 continua padrao.
- Opus continua opt-in.
- Opus upstream usa 16 kHz mono, 60 ms, 32 kbps.
- AEC device-side nao e promovivel no hardware atual sem referencia de playback.
- Follow-up automatico fica em standby ate a base de audio v2 estar estavel.
- Barge-in sem wake por VAD fica fora ate AEC/AFE e no-echo estarem robustos.
- Nao copiar WebSocket Xiaozhi inteiro agora.
- Nao copiar C++/std::vector para firmware C17 do NoiseBot.

## Proxima Implementacao Permitida

A Fase B ja criou apenas headers, CMake e fontes inativas dos novos
componentes. A proxima mudanca de codigo deve ser a Fase C:

- ativar `audio_io_service_v2` somente como probe explicito;
- ler mic e alimentar speaker com silencio sem tocar wake, bridge ou playback
  atual;
- expor metricas de chunks, falhas I2S, RMS/peak e heap;
- manter rollback imediato para o pipeline v1.

Qualquer mudanca em wake, VAD thresholds, state machine, barge-in ou follow-up
antes disso deve ser considerada fora de escopo.
