# Voice Audio v2 - Proximas Fases Pos-Opus

Data: 2026-05-31
Status: roadmap operacional pos-fechamento da migracao Opus v2.
Branch de referencia: `voice-reference-architecture`.

Este documento continua `VOICE_AUDIO_V2_ARCHITECTURE.md` depois do fechamento
da migracao Opus v2. A meta agora nao e "colocar mais teste por teste": e
terminar a separacao arquitetural inspirada em Xiaozhi/StackChan sem quebrar o
NoiseBot que ja conversa bem.

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

## Referencia Xiaozhi/StackChan

O que continuar copiando como principio:

- I/O de audio separado de codec e de policy.
- Task dedicada para codec Opus, com filas curtas.
- Processor plugavel entre microfone e codec.
- Estados explicitos de conversa: idle, listening, speaking, abort/cancel.
- Abort/cancel limpo quando wake acontece durante fala.
- Capabilities de audio explicitas no protocolo.

O que nao copiar diretamente:

- `AUDIO_INPUT_REFERENCE=true` do CoreS3/StackChan. O NoiseBot atual usa
  INMP441 + MAX98357A e nao possui canal limpo de referencia do speaker.
- AEC device-side como feature geral. No NoiseBot atual, AEC fica bloqueado ate
  existir referencia real ou desenho server-side com timestamps.
- Input/output 24 kHz do CoreS3 como requisito. O caminho atual do NoiseBot e
  16 kHz, mono, e ja esta alinhado ao STT e ao Opus upstream.
- WebSocket/MQTT inteiro do Xiaozhi. O bridge TCP local ja funciona e deve ser
  evoluido de forma incremental.
- C++/STL do Xiaozhi/StackChan no firmware C17 do NoiseBot.

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
- Hardware ainda precisa de flash para validar os novos campos em
  `/api/audio/playback-v2` durante uma resposta real.

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

Objetivo: criar o processor plugavel inspirado no Xiaozhi, usando AFE/VAD/NS
como diagnostico ou opt-in, sem prometer AEC.

Entregas:

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

Aceite:

- Nenhuma fila/drops/erro de codec fica ambigua.
- Corte visual, corte de audio e falha de TTS ficam separados em metricas.
- Toda regressao real vira caso de teste antes de novo ajuste.

## Ordem Recomendada

1. Fase I: playback v2 como dono gradual do downlink.
2. Fase M parcial: checklist/health de release para proteger o que ja ficou
   bom.
3. Fase J: voice activity v2 em shadow/opt-in, sem AEC.
4. Fase K: capture session v2 assume upstream por flag.
5. Fase L: policy conversacional avancada, so depois de no-echo e captura
   estarem estaveis.

Essa ordem segue a licao central do Xiaozhi/StackChan: separar I/O, playback,
processor, codec e policy. No NoiseBot, a prioridade imediata e diminuir o
acoplamento do `audio_service.c` sem trocar wake, VAD, AEC e follow-up no mesmo
movimento.
