# Voice Audio v2 - Release Checklist / Health

Data: 2026-06-01
Status: Fase M parcial, operacional, sem alteracao de firmware C.
Branch de referencia: `voice-reference-architecture`.

Este checklist protege o estado bom atual antes de novas mudancas grandes no
Voice Audio v2. Ele cobre Opus v2 como default local do server, Playback v2
como dono da fila curta de downlink SAY, Capture v2 desligado por padrao, regressao
`barge-live`/`no-echo-live` e completude TTS/texto.

Escopo negativo:

- Nao mexer em wake word, thresholds de VAD, AEC, follow-up, `audio_service.c`
  ou HAL.
- Nao ligar `voice_audio_v2_capture_enabled` fora de teste explicito com
  rollback.
- Nao remover nem enfraquecer rollback PCM16.
- Nao tratar corte visual de `TEXT_SCROLL` como falha de audio sem evidencias
  de TTS/SAY/playback.

## Evidencias De Base

Fontes lidas nesta preparacao:

- `docs/VOICE_AUDIO_V2_NEXT_PHASES.md`
- `docs/VOICE_AUDIO_V2_ARCHITECTURE.md`
- `docs/VOICE_PIPELINE.md`
- `docs/OBSIDIAN_VOICE_AUDIO_V2_KNOWLEDGE.md`
- `server/noisebot_server/internal/ops/firmware_diag.py`
- `server/noisebot_server/cli.py`
- `server/noisebot_server/internal/ops/metrics.py`
- `server/noisebot_server/internal/ops/status.py`
- `bridge/noisebot_bridge/voice_check.py`

Evidencias ja registradas nos docs do projeto:

- Opus v2 fechado como default local do server por
  `NOISEBOT_AUDIO_DEFAULT_CODEC=opus-v2`, com rollback por
  `NOISEBOT_AUDIO_DEFAULT_CODEC=pcm16` + restart ou
  `codec-v2 transport-disable`.
- `codec-v2 health` live retornou `healthy=true`, `status=ok`, zero drops,
  fila egress zero e `opus_codec_error=0`.
- Playback v2 foi validado apos flash primeiro como observador SAY e depois
  como dono da fila curta SAY: o handoff parcial confirmou
  `bridge_say_queue_owner=true`, fila de 16 chunks, turno real com 283 chunks
  recebidos/tocados, fila final zero, zero drops, `SAY_END` confirmado e
  `ESP_OK`.
- `capture-v2 status` permaneceu desligado: `real_capture_enabled=false`,
  `session_active=false`, `state=IDLE_SESSION`, `last_error=ESP_OK`.
- `barge-live --codec opus-v2` e `no-echo-live --codec opus-v2` passaram em
  hardware com zero drops e rollback automatico.
- Resposta longa mostrou TTS/playback completo (`tts_completed=true`,
  `tts_say_end_sent=true`, 589 chunks SAY) e problema visual separado em
  `TEXT_SCROLL`, hoje paginado pelo server.

## Comandos Base

Use `--host` ou `NOISEBOT_ROBOT_HTTP_URL` conforme o ambiente local. Quando o
server ja esta rodando em Opus v2, estes comandos nao devem mudar firmware C.

```powershell
noisebot_server --host 192.168.1.30 debug codec-v2 health --json
noisebot_server --host 192.168.1.30 debug capture-v2 status --json
noisebot_server --host 192.168.1.30 debug codec-ab --repeat 3 "me diga uma curiosidade" --json
noisebot_server --host 192.168.1.30 debug barge-live "me conte uma historia longa" --codec opus-v2 --json
noisebot_server --host 192.168.1.30 debug no-echo-live "me conte uma historia longa" --codec opus-v2 --json
```

Checklist sem hardware:

```powershell
cmd.exe /c "set PYTHONPATH=D:\Projetos\Noisebot\bridge&& C:\Users\Tauser\AppData\Local\Python\pythoncore-3.14-64\python.exe -m pytest bridge\tests\test_voice_check.py bridge\tests\test_firmware_audio_service_contract.py bridge\tests\test_firmware_bridge_contract.py"
cmd.exe /c "set PYTHONPATH=D:\Projetos\Noisebot\server&& C:\Users\Tauser\AppData\Local\Python\pythoncore-3.14-64\python.exe -m pytest server\tests\test_server_facade.py"
```

## Gate 0 - Escopo Limpo

Verde:

- Nenhum arquivo C/C++ de firmware foi alterado para esta Fase M parcial.
- `audio_service.c`, HAL, wake, VAD, AEC e follow-up continuam fora do diff.
- Docs registram comandos/evidencias quando forem atualizados.

Bloqueia release:

- Diff toca `components/services/audio_service/audio_service.c`.
- Diff toca `components/nb_hal/audio_hal.*` ou outro HAL de audio.
- Diff altera wake/VAD/AEC/follow-up sem plano, teste e rollback proprios.

Comando de auditoria:

```powershell
git diff --name-only -- components server bridge docs
```

## Gate 1 - Codec v2 / Opus

Verde:

- `codec-v2 health` retorna `ok=true`, `healthy=true`, `status=ok`.
- `packet_drops=0`, `opus_egress_packet_drops=0`,
  `opus_egress_queue_count=0`, `opus_codec_error=0`.
- Worker esta coerente com o transporte ativo: se Opus esta ativo, worker
  `running`; se rollback PCM16 foi acionado, worker parado e filas limpas.
- Rollback PCM16 esta documentado e testavel.

Bloqueia release:

- Qualquer drop de pacote Opus.
- `opus_codec_error != 0`.
- Fila egress pendente que nao limpa com `codec-v2 egress-drain`.
- Opus ativo sem worker ativo.

Rollback:

```powershell
noisebot_server --host 192.168.1.30 debug codec-v2 transport-disable --json
```

ou reiniciar o server com:

```powershell
set NOISEBOT_AUDIO_DEFAULT_CODEC=pcm16
```

## Gate 2 - Playback v2 Observador SAY

Verde:

- `/api/audio/playback-v2` mostra `bridge_say_observer=true` e, apos o
  handoff parcial da Fase I, `bridge_say_queue_owner=true`.
- Pos-flash do handoff parcial validado: turno real com 283 chunks SAY
  recebidos/tocados, fila final zero, zero drops, `SAY_END` confirmado e
  `ESP_OK`.
- Durante turno conversacional real, `say_chunks_received` cresce junto com
  `say_chunks_played`.
- `say_queue_count=0` ao final do turno.
- `say_chunks_dropped` e `say_chunks_dropped_listening` nao aumentam no caminho
  representativo do orquestrador.
- `say_cancel_count` so aumenta quando ha cancel/barge-in real.
- Barge-in pos-handoff validado por metrica em hardware: `/ai/metrics`
  registrou `outcome=interrupted`, `discard_reason=barge_in` e
  `interruption_cancel=3.5 ms`; Playback v2 permaneceu com fila final zero e
  `ESP_OK`.
- Rodada controlada pos-restart validada em hardware: `ww -> que horas sao?`
  respondeu como `local_time`; `ww -> me conte uma historia longa -> ww ->
  pare` registrou o turno longo como `outcome=interrupted` /
  `discard_reason=barge_in`, cancelamento p50 2,6 ms / p95 3,2 ms, e o turno
  seguinte reconheceu `Pare.`. Playback v2 terminou com `say_queue_count=0`,
  `say_cancel_count=2`, `say_chunks_cancelled=28`, `last_error=ESP_OK`, e
  `/ai/status` confirmou Opus ativo.
- `last_error=ESP_OK`.

Bloqueia release:

- Chunks SAY recebidos sem contagem correspondente de tocados no turno normal.
- Fila SAY permanece nao-zero apos `SAY_END`.
- Drops aparecem no caminho do orquestrador sem motivo de cancel/listening.
- Audio antigo toca depois de cancelamento.

Nota: `/api/profile/test-voice` pode gerar drops por nao passar pelo
`OutputScheduler` conversacional. Para aceite de release, use turno real pelo
orquestrador ou `debug transcript`.

## Gate 3 - Capture v2 Desligado

Verde:

- `capture-v2 status` retorna `ok=true`.
- `real_capture_enabled=false`.
- `session_active=false`.
- `state=IDLE_SESSION`.
- `last_error=ESP_OK` ou `error=ESP_OK`.

Bloqueia release:

- `voice_audio_v2_capture_enabled` ficou ligado sem teste explicito.
- Sessao v2 ativa em idle.
- `VOICE_START/AUDIO_CHUNK/VOICE_END` passou a depender do v2 sem fase K.

Comando:

```powershell
noisebot_server --host 192.168.1.30 debug capture-v2 status --json
```

## Gate 4 - PCM16 Rollback

Verde:

- `codec-ab --repeat 3` passa PCM16 e Opus v2.
- PCM16 tem `ok=true`, `transcript_quality=good` e sem regressao semantica
  grosseira.
- Ao final do A/B, o status do Codec v2 fica sem drops e com fila egress zero.

Bloqueia release:

- Opus so passa quando PCM16 falha.
- Rollback deixa worker/fila em estado sujo.
- Server nao volta a reportar PCM16 quando `transport-disable` ou env `pcm16`
  sao usados.

## Gate 5 - Barge-Live

Verde:

- `barge-live --codec opus-v2` retorna `ok=true`.
- `outcome=interrupted`.
- `discard_reason=barge_in`.
- `interruption_cancel_ms < 200`.
- Pacotes Opus drenados > 0, drops = 0, enable/disable ok.

Bloqueia release:

- Barge cancela a fala velha, mas nao abre turno limpo.
- Audio antigo continua tocando apos cancel.
- Drops aparecem durante o teste.

## Gate 6 - No-Echo-Live

Verde:

- `no-echo-live --codec opus-v2` retorna `ok=true`.
- `unexpected_turn_id=null` durante a janela silenciosa.
- Drops = 0, enable/disable ok.
- O robo volta ao baseline de turno sem abrir fala fantasma.

Bloqueia release:

- Novo turno surge sem wake/fala do usuario.
- O proprio TTS reabre captura.
- Qualquer tentativa de "corrigir" isso reabrindo AEC/follow-up sem fase
  propria.

## Gate 7 - TTS / Texto

Verde em `/ai/metrics`:

- `tts_completed=true`.
- `tts_say_begin_sent=true`.
- `tts_say_end_sent=true`.
- `tts_chunks_sent > 0`.
- `tts_pcm_bytes_sent > 0`.
- Se `text_scroll_truncated=true`, entao `text_scroll_pages_sent >=
  text_scroll_pages > 1` ou o diagnostico deixa claro que e limite visual, nao
  corte de audio.
- `voice_alert=null` para turno normal concluido.

Bloqueia release:

- `tts_completed=false`.
- `tts_say_end_sent=false` sem cancel/barge-in.
- `text_scroll_pages_sent < text_scroll_pages`.
- Diagnostico mistura corte visual, falha de TTS e queda de Opus sem separar
  metricas.

## Registro Da Rodada

Para cada release local de voz, registrar:

- Data, branch e hash do firmware/server testado.
- Comandos executados e arquivos JSON/Markdown gerados.
- Resultado de `codec-v2 health`, `capture-v2 status`, Playback v2 SAY,
  `codec-ab`, `barge-live`, `no-echo-live` e `/ai/metrics`.
- Codec ativo no inicio e no fim.
- Se rollback PCM16 foi exercitado.
- Qualquer falha real deve virar teste, replay ou checklist antes de novo
  ajuste.

## Aceite Da Fase M Parcial

Esta Fase M parcial esta pronta quando:

- O checklist acima esta referenciado pelos docs de voz.
- Os gates protegem Opus v2, Playback v2 dono da fila SAY, Capture v2 desligado,
  barge/no-echo e completude TTS/texto.
- O documento deixa explicito que wake, VAD, AEC, follow-up, `audio_service.c`
  e HAL ficam fora do escopo.
- PCM16 rollback continua como criterio obrigatorio.

## Gate Extra - Antes De Validar Voice Activity v2

O primeiro incremento da Fase J e apenas shadow/probe:

- Confirmar `/api/audio/activity-v2` antes e depois de iniciar
  `/api/audio/activity-v2/shadow`.
- Durante uma resposta real, `muted_frames` pode crescer porque o playback esta
  ativo; isso nao deve abrir sessao nem afetar wake.
- Confirmar `codec-v2 health`, `/api/audio/capture-v2` e
  `/api/audio/playback-v2` depois do probe.
- Qualquer ajuste de threshold so entra depois de replay/harness ou evidencia
  numerica comparavel; nao calibrar por percepcao solta.
