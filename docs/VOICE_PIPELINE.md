# Voice Pipeline

Este documento fixa o contrato de áudio conversacional do NoiseBot. A regra é
simples: firmware, bridge e server devem concordar nos mesmos limites antes de
qualquer otimização de STT/TTS.

## Contrato v1

- Formato: PCM16 little-endian, mono, 16 kHz.
- Chunk: 256 samples, 512 bytes, 16 ms.
- Modo de escuta: `auto`.
- Duração máxima de fala: 10 s.
- Áudio mínimo para STT: 8000 samples, 500 ms.
- Áudio máximo para STT: 160000 samples, 10 s.
- Silêncio final: 900 ms.
- Pre-roll no firmware: 320 ms.

O firmware anuncia esse contrato no `HELLO` do bridge e o server valida o
tamanho dos chunks recebidos. Frames fora do contrato são descartados antes de
entrar no pipeline de STT.

## Decisões

- O caminho atual permanece PCM local. Opus é uma evolução futura, não requisito
para estabilidade local.
- Supressão de ruído em Python fica desligada por padrão. Nos testes práticos
ela piorou a transcrição e aumentou risco de watchdog.
- Backpressure persistente no bridge encerra a sessão. Áudio atrasado ou
picotado não deve chegar ao STT como se fosse fala válida.
- O teto de 10 s é compartilhado entre firmware e server. Sessões longas são
descartadas sem chamar STT.

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

Status: em andamento. O resumo da última sessão, o histórico recente e os alertas
de descarte/falha já são registrados no server, expostos em `/ai/metrics` e
exibidos no dashboard dev.

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

Status: iniciada. O barge-in agora registra o turno antigo como interrompido,
remove frames de fala pendentes da fila TCP antes do `SPEECH_CANCEL`, tolera
falha de cancelamento do firmware e abre o próximo turno com watchdog ativo. O
follow-up deixou de depender de `?` no texto exibido: o server manda
`FOLLOWUP_ARM` por `SESSION` quando a resposta real pede continuação, e o prompt
de wake vazio não arma nova escuta. Os demais encerramentos agora usam contrato
terminal explícito (`SESSION_DONE`, `SESSION_ERROR` ou `FOLLOWUP_CANCEL`) para
não deixar estado pendente no firmware. O prompt de wake vazio permanece limitado
a uma vez por sequência até existir fala útil.

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
- Interromper TTS com fala cancela o áudio antigo e inicia novo turno limpo.
- Follow-up funciona sem reacordar o robô artificialmente.

### Fase 4 — Qualidade de Entrada Sem Denoise Arriscado

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

Critérios de aceite:

- 20 comandos reais gravados e comparados.
- Taxa de comandos entendidos melhora sem aumentar watchdog.
- Nenhum filtro novo entra sem gravação antes/depois.

### Fase 5 — Pipeline AFE no Firmware

Objetivo: absorver a parte mais valiosa do Xiaozhi: processamento de voz no ESP,
mas sem sacrificar câmera, TTS e estabilidade.

Mudanças:

- Criar componente experimental `audio_processor_service`.
- Avaliar AFE `AFE_TYPE_VC` em modo high performance.
- Testar VADN/NSNET se modelos estiverem disponíveis.
- Medir RAM com:
  - câmera ativa;
  - TTS ativo;
  - dashboard conectado;
  - bridge conectado;
  - SD logging ativo.
- Manter fallback para o VAD atual.

Critérios de aceite:

- Sem regressão de câmera 640x480.
- Sem queda do TTS.
- Sem watchdog em 30 minutos de uso misto.
- Ganho real de STT comprovado por amostras comparáveis.

### Fase 6 — Opus/Frames de 60 ms

Objetivo: reduzir banda e aproximar o protocolo do Xiaozhi quando fizer sentido.

Mudanças:

- Adicionar negociação de codec no `HELLO`:
  - `pcm16` como baseline;
  - `opus` como opcional.
- Implementar Opus apenas atrás de feature flag.
- Server deve aceitar os dois formatos durante a transição.
- Medir latência e CPU antes de tornar padrão.

Critérios de aceite:

- PCM continua funcionando como fallback.
- Opus não aumenta latência perceptível.
- Filas continuam curtas e previsíveis.
- Nenhuma mudança de codec quebra dashboard, STT ou TTS.

### Fase 7 — AEC e Modo Realtime

Objetivo: permitir barge-in e conversa mais natural enquanto o robô fala.

Mudanças:

- Avaliar AEC no firmware apenas depois do AFE estável.
- Usar referência do speaker se o caminho de áudio permitir.
- Ativar `realtime` apenas com AEC validado.
- Server deve distinguir fala do usuário de eco do TTS.

Critérios de aceite:

- Usuário consegue interromper o robô falando por cima.
- O próprio TTS não reabre escuta falsa.
- Sem loops de escuta/resposta.

### Fase 8 — Produto e Regressão Contínua

Objetivo: manter o ciclo funcionando conforme novas features entram.

Mudanças:

- Criar replay de sessões reais boas e ruins.
- Testar localmente:
  - wake sem fala;
  - fala curta;
  - fala longa;
  - ruído ambiente;
  - TTS interrompido;
  - câmera ativa durante fala;
  - bridge reconectando.
- Colocar esses cenários em checklist antes de release.

Critérios de aceite:

- Nenhuma release sai sem passar o replay básico.
- Toda regressão de voz vira caso de teste.
- Dashboard dev mostra causa provável antes do usuário precisar ler log.

## Ordem Recomendada

1. Fechar Fase 2 no dashboard dev.
2. Fechar Fase 3 no firmware/server.
3. Coletar amostras reais da Fase 4.
4. Só então iniciar AFE experimental.
5. Só depois de AFE estável avaliar Opus.
6. Só depois de AFE/Opus maduros avaliar AEC/realtime.

Essa ordem evita a armadilha de trocar codec, VAD, AEC e STT ao mesmo tempo. O
fim desejado é ambicioso, mas cada fase precisa ter medição própria para o robô
continuar utilizável todos os dias.
