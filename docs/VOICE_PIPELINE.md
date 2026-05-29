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

- O caminho atual permanece PCM local. Opus é uma evolução futura, não requisito
para estabilidade local.
- Supressão de ruído em Python fica desligada por padrão. Nos testes práticos
ela piorou a transcrição e aumentou risco de watchdog.
- Backpressure persistente no bridge encerra a sessão. Áudio atrasado ou
  picotado não deve chegar ao STT como se fosse fala válida.
- O teto de 10 s é compartilhado entre firmware e server. Sessões longas são
  descartadas sem chamar STT.
- O server rechunkeia a saída TTS para frames exatos de 512 bytes antes de
  enviar `SAY`; chunks maiores vindos do gerador de áudio nunca cruzam o
  contrato TCP com o firmware.

## Referência Xiaozhi/StackChan

O Xiaozhi usa Opus 16 kHz mono com frames de 60 ms, filas curtas e processamento
de voz via AFE quando disponível. O que absorvemos neste estágio é a disciplina
de contrato, filas e limites. A migração para AFE/Opus deve ser uma fase própria,
com medição de RAM junto da câmera e do TTS.

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

Objetivo: absorver a parte mais valiosa do Xiaozhi: processamento de voz no ESP,
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

Status: concluida como modo experimental opt-in. PCM16 continua sendo o padrão
seguro (`audio.format=pcm16`, `pcm16=true`, `opus=false`). O firmware agora
consegue iniciar um worker Opus persistente, codificar PCM real em frames de
60 ms (`NB_BRIDGE_OPUS_FRAME_MS=60`, `NB_BRIDGE_OPUS_FRAME_SAMPLES=960`),
enfileirar pacotes Opus em PSRAM e, quando a flag experimental é ligada,
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

Objetivo: reduzir banda e aproximar o protocolo do Xiaozhi quando fizer sentido.

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
- `bridge/tests/test_firmware_bridge_contract.py`: firmware real mantém PCM16
  como contrato padrão e exige flag explicita para Opus.
- `bridge/tests`: 153 testes verdes após a integração.
- `server/tests`: 92 testes verdes após adicionar o harness `opus-live` e o
  fallback de decode Opus antes do `HELLO` dinâmico.
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
- [ ] Manter Opus como opt-in até A/B maior de latência/CPU antes de promover
  como padrão.

### Fase 7 — AEC e Modo Realtime

Objetivo: permitir barge-in e conversa mais natural enquanto o robô fala.

Mudanças:

- Avaliar AEC no firmware apenas depois do AFE estável.
- Usar referência do speaker se o caminho de áudio permitir.
- Ativar `realtime` apenas com AEC validado.
- Server deve distinguir fala do usuário de eco do TTS.
- O AEC agora tem probe próprio em `/api/audio/processor/aec/probe`: alinhado
  ao Xiaozhi, ele cria um AFE `MR` de voz (`AFE_TYPE_VC` +
  `AEC_MODE_VOIP_HIGH_PERF`), mede PSRAM/heap interno/DMA e destrói antes de
  retornar. O caminho principal não ativa AEC se a margem de heap estiver
  baixa.

Nota de bancada em 2026-05-27: a tentativa de promover WakeNet `MR + AEC`
direto para runtime compilou, mas no hardware causou pressão de memória
observada como `sdmmc_read_sectors: not enough mem`, degradação do SD e queda
de WiFi/bridge. A decisão correta é manter AEC fora do caminho principal até o
probe passar com margem e sem regressão em SD/WiFi.

Critérios de aceite:

- Usuário consegue interromper o robô falando por cima.
- O próprio TTS não reabre escuta falsa.
- Sem loops de escuta/resposta.
- Sem regressão de SD/WiFi/bridge durante ou após o probe.

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

## Ordem Recomendada

1. Coletar amostras reais da Fase 4.
2. Só então iniciar AFE experimental.
3. Só depois de AFE estável avaliar Opus.
4. Só depois de AFE/Opus maduros avaliar AEC/realtime.

Essa ordem evita a armadilha de trocar codec, VAD, AEC e STT ao mesmo tempo. O
fim desejado é ambicioso, mas cada fase precisa ter medição própria para o robô
continuar utilizável todos os dias.
