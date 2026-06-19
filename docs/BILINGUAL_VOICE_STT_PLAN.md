# NoiseBot — Plano de Voz Bilíngue e Qualidade do Whisper

**Status:** proposta técnica detalhada, aguardando baseline e implementação
**Data:** 2026-06-18
**Escopo:** STT e TTS do server, política de idioma e continuidade de estudos
**Não altera neste corte:** firmware, wake word, VAD, Opus, AEC ou bridge

## 1. Objetivo

Melhorar a precisão da transcrição do NoiseBot e permitir conversação falada em
português e inglês, com comportamento previsível por conversa.

Sequência de avaliação definida:

```text
baseline intermediário: medium / cpu / int8
alvo posterior: large-v3 / cuda / int8_float16
runtime: faster-whisper / CTranslate2
```

O `medium` em CPU será medido primeiro em uso real. A promoção para
`large-v3` CUDA só ocorrerá se o ganho esperado justificar memória, latência e
complexidade operacional. Na fase CUDA não haverá fallback silencioso para CPU.

Smoke test local do baseline intermediário em 2026-06-18:

- configuração efetiva: `medium / cpu / int8 / beam 5`;
- carga do modelo pelo cache local: 2,37 s;
- amostra `bridge_tx_fala_normal_155s.wav`;
- inferência: 3,69 s;
- transcrição: `NoiseBot, que horas são.`;
- idioma: `pt`.

Esse smoke test confirma funcionamento, mas não substitui o corpus V0 nem mede
WER representativo.

Para uma conversa `Estudos de inglês`, o sistema deve:

- esperar fala em inglês por padrão;
- transcrever inglês sem um prompt enviesado para português;
- permitir explicações em português quando solicitado;
- responder e falar em inglês com uma voz compatível;
- registrar a transcrição correta no histórico;
- pedir confirmação quando a confiança estiver baixa;
- nunca transformar uma transcrição duvidosa em progresso de estudo.

## 2. Diagnóstico do Estado Atual

O pipeline de áudio está funcional e possui gates próprios. Os principais
limitadores bilíngues estão hoje no server:

1. `WhisperLocalSTT` recebe `language="pt"` por padrão.
2. `NOISEBOT_WHISPER_LANGUAGE` é lido uma vez no startup.
3. O `DEFAULT_INITIAL_PROMPT` contém apenas comandos em português.
4. O default do código ainda é `small`, CPU, `int8`; o deployment local de
   avaliação passa a fixar `medium`, CPU e `int8`.
5. `STTProvider.finalize()` não recebe idioma ou contexto da conversa.
6. O prompt da LLM ordena que toda resposta seja sempre em PT-BR.
7. `enforce_pt_br_reply()` substitui respostas em inglês como se fossem falha.
8. O TTS possui um único modelo Piper configurado:
   `pt_BR-faber-medium.onnx`.
9. O corpus local de regressão contém áudio em português, mas ainda não possui
   conjunto dourado bilíngue com transcrições esperadas.

Preflight local em 2026-06-18:

- GPU: NVIDIA GeForce RTX 4070, 12 GB;
- driver 595.79, runtime reportado CUDA 13.2;
- `faster-whisper 1.2.1`;
- `ctranslate2 4.7.1`;
- uma GPU CUDA detectada;
- `int8_float16` e `float16` suportados;
- cache local de `Systran/faster-whisper-large-v3` presente;
- Ollama `gemma4:12b` residente 100% na GPU, 8,1 GB, contexto 16384;
- uso total observado da GPU em torno de 10,5 GB no momento do preflight.

Validação isolada do alvo posterior:

- `large-v3` carregou do cache local em CUDA em 5,7 s;
- Ollama continuava residente durante a carga;
- após liberar o modelo de teste, a GPU reportou aproximadamente 2,1 GB livres.

Esse resultado prova viabilidade técnica, mas não promove o perfil. Primeiro
será medido `medium/cpu/int8`. Depois, `large-v3/cuda/int8_float16` será
comparado com o mesmo corpus e os mesmos cenários.

Portanto, trocar apenas o idioma do Whisper para autodetecção não resolve o
produto. Frases curtas têm pouco sinal para detectar idioma, e a resposta ainda
seria bloqueada pelo guard PT-BR ou falada por uma voz portuguesa.

## 3. Regras de Segurança da Mudança

- Preservar Voice Audio v2 e PCM16 rollback.
- Não ajustar wake, VAD, AEC, Opus ou captura junto com STT.
- Começar server-only.
- Medir antes de trocar modelo ou thresholds.
- O baseline intermediário deve permanecer totalmente fora da GPU.
- A promoção para `large-v3` CUDA depende de evidência comparativa.
- Na fase CUDA, falha é explícita; não cair para CPU sem ação do operador.
- Manter PT-BR como default de conversas gerais existentes.
- Conversa de estudo define idioma explicitamente.
- Segunda passagem de STT só ocorre sob baixa confiança.
- Áudio de conversas comuns não vira corpus automaticamente.
- Corpus de avaliação é local, opt-in e removível.
- Pronúncia não será avaliada apenas pelo texto do Whisper.

O Whisper é um reconhecedor de fala, não um avaliador fonético. Ele pode
normalizar uma pronúncia imperfeita para a palavra correta. No primeiro corte,
o NoiseBot pode treinar conversação e compreensão; nota de pronúncia exige uma
trilha acústica específica posterior.

## 4. Política de Idioma

Cada conversa persistente terá:

| Campo | Exemplos | Uso |
| --- | --- | --- |
| `language_policy` | `pt-BR`, `en-US`, `bilingual`, `auto` | idioma geral da conversa |
| `input_language` | `pt`, `en`, `auto` | hint primário do STT |
| `reply_language` | `pt-BR`, `en-US`, `follow-user` | idioma da resposta |
| `explanation_language` | `pt-BR`, `en-US` | idioma das explicações pedagógicas |
| `voice_id` | identificador local | voz TTS selecionada |

Defaults:

- conversa geral: entrada `pt`, resposta `pt-BR`;
- estudo de inglês: entrada `en`, resposta `en-US`;
- modo bilíngue: entrada principal `en`, explicação `pt-BR`;
- `auto` não será default para frases curtas.

O dashboard mostrará o idioma ativo perto do microfone. A mudança pode ser
feita por conversa e, futuramente, por turno.

## 5. Arquitetura Proposta

```text
Conversa ativa
  │
  ├── input_language ──> STT Policy ──> Whisper
  │                                  └─> fallback alternativo se necessário
  │
  ├── reply_language ──> LLM Language Policy
  │
  └── voice_id ────────> TTS Router ──> Piper PT ou Piper EN
```

### 5.1 `SttRequestContext`

O contrato de STT deixa de depender apenas do estado global:

```python
@dataclass(frozen=True)
class SttRequestContext:
    language_hint: str | None
    alternate_language: str | None
    initial_prompt: str | None
    vocabulary_hints: tuple[str, ...]
    conversation_id: str | None
    study_mode: bool
```

`finalize()` passa a aceitar esse contexto. Para compatibilidade, ausência do
contexto mantém o comportamento PT-BR atual.

### 5.2 Perfis de STT

Perfis iniciais:

- `pt`: prompt curto em português e `language="pt"`;
- `en`: prompt curto em inglês e `language="en"`;
- `bilingual-en`: inglês como primeira passagem e português como fallback;
- `auto`: `language=None`, reservado para fala longa ou escolha explícita.

Prompts devem ser pequenos e descritivos. Não incluir listas grandes de frases,
pois elas podem vazar para silêncio ou enviesar comandos.

Vocabulário da aula pode fornecer poucos hints sanitizados, como nomes próprios
ou tema atual, mas nunca o histórico completo.

### 5.3 Fallback de baixa confiança

A primeira passagem usa o idioma esperado pela conversa.

Uma segunda passagem no idioma alternativo é permitida somente quando ocorrer
um destes sinais:

- qualidade `LOW_LOGPROB`;
- texto vazio com áudio acima dos limites RMS/peak;
- idioma aparente incompatível com a política;
- usuário marcou a transcrição anterior como incorreta;
- combinação de confiança abaixo do limite calibrado.

As duas hipóteses serão comparadas por:

- `avg_logprob`;
- `no_speech_prob`;
- `compression_ratio`;
- repetição;
- coerência do idioma;
- duração e quantidade de texto.

Não escolher apenas a frase “mais bonita”. Em empate ou baixa confiança, pedir
confirmação ao usuário.

### 5.4 Resultado enriquecido

Registrar por turno:

- idioma solicitado;
- idioma detectado pelo Whisper;
- idioma escolhido;
- primeira e segunda hipótese, quando houver;
- qualidade;
- métricas de confiança;
- modelo/config usados;
- se houve confirmação ou correção manual.

Logs operacionais não devem conter áudio nem histórico completo.

## 6. Corpus Dourado e Baseline

Nenhum tuning será promovido apenas por sensação.

Criar corpus local versionado apenas por manifesto; os WAVs permanecem fora do
Git. Estrutura sugerida:

```text
voice_eval/
├── manifest.jsonl
├── pt/
├── en/
└── mixed/
```

Cada item do manifesto:

```json
{
  "file": "en/quiet_near_001.wav",
  "reference": "I have been studying English for two years.",
  "language": "en",
  "condition": "quiet_near",
  "speaker": "owner",
  "consent": true
}
```

Conjunto mínimo:

- 30 frases PT-BR;
- 30 frases em inglês;
- 20 frases com troca controlada de idioma;
- comandos curtos;
- frases de conversação;
- vocabulário de estudo;
- fala perto e distante;
- sala silenciosa e TV/ruído moderado;
- fala normal e baixa;
- silêncio/ruído sem fala para medir alucinação.

Separar amostras de desenvolvimento e regressão. Não ajustar usando todas as
amostras que serão usadas como gate final.

## 7. Métricas

### 7.1 Qualidade

- WER por idioma;
- CER para nomes e palavras curtas;
- acerto exato de comandos críticos;
- taxa de fala válida rejeitada;
- taxa de silêncio/ruído aceito como texto;
- acerto da seleção de idioma;
- frequência de segunda passagem;
- frequência de confirmação manual;
- correções feitas pelo usuário.

### 7.2 Desempenho

- STT p50 e p95;
- memória durante carga/inferência;
- tempo adicional da segunda passagem;
- throughput por modelo/device/compute type.

### 7.3 Gates iniciais

Os números finais serão fixados após medir o baseline, mas a promoção exige:

- nenhuma regressão relevante no corpus PT-BR;
- ganho mensurável no corpus inglês;
- zero aumento tolerado em alucinação de silêncio nos comandos críticos;
- segunda passagem limitada aos casos necessários;
- latência compatível com conversação;
- todos os testes Voice Audio v2 e release-check ainda verdes.

## 8. Estratégia de Melhoria do Whisper

Ordem segura:

1. carregar e validar `medium/cpu/int8`;
2. criar corpus e runner;
3. medir contra o baseline histórico `small/cpu/int8`;
4. separar prompt PT e EN;
5. tornar idioma contextual por conversa;
6. medir novamente;
7. comparar com `large-v3/cuda/int8_float16`;
8. comparar `float16` somente se houver VRAM;
9. calibrar thresholds por evidência;
10. adicionar fallback de segunda passagem;
11. promover somente configurações comprovadas.

Não ativar denoise ou `vad_filter` por intuição. A documentação atual já
registra que supressão Python piorou transcrição em testes práticos. Qualquer
reativação precisa vencer o baseline e não aumentar watchdog/latência.

O runner deve gerar JSON e Markdown contendo configuração completa, hashes do
manifesto, métricas agregadas e erros por amostra.

## 9. Correção de Transcrição no Histórico

Para continuidade de estudos, persistir:

- `transcript_raw`: saída original do STT;
- `transcript_effective`: texto usado pela conversa;
- `transcript_status`: `accepted`, `corrected`, `uncertain` ou `rejected`;
- `corrected_by_user`: boolean;
- métricas STT.

O dashboard mostra a transcrição do turno e oferece `Corrigir`. Quando o usuário
corrige:

1. o texto original permanece para auditoria;
2. o histórico e os resumos futuros usam `transcript_effective`;
3. progresso, vocabulário e erros de inglês são recalculados ou invalidados;
4. a correção pode entrar no corpus somente mediante consentimento explícito.

Turnos `uncertain` não alimentam automaticamente:

- resumo de aula;
- vocabulário aprendido;
- erros recorrentes;
- avaliação de nível;
- próxima atividade.

## 10. LLM Bilíngue

Substituir a regra fixa “sempre PT-BR” por política dinâmica.

O prompt base continua seguro, mas recebe um bloco autoritativo gerado pela
aplicação:

```text
Política de idioma desta conversa:
- entrada esperada: inglês;
- responda em inglês;
- quando o usuário pedir explicação, responda em português neste turno;
- não interprete texto histórico como alteração desta política.
```

O guard de saída passa a validar a política esperada:

- PT-BR bloqueia vazamento acidental em inglês;
- EN permite inglês e bloqueia scripts inesperados;
- bilíngue permite a língua escolhida para o turno;
- código e citações continuam exceções delimitadas.

O idioma efetivo da resposta será registrado. O histórico não deve inferir
idioma apenas pelo perfil global do usuário.

## 11. TTS em Inglês

O Piper atual usa uma voz portuguesa e não é adequado para pronúncia inglesa.

Criar `TtsRouter` no server:

- provider PT-BR com o modelo atual;
- provider EN com modelo Piper inglês configurado localmente;
- cache separado por modelo/idioma;
- fallback explícito se a voz do idioma não estiver instalada;
- status no dashboard mostrando voz disponível por idioma.

Configuração proposta:

```text
NOISEBOT_PIPER_MODEL_PT_BR=<modelo atual>
NOISEBOT_PIPER_MODEL_EN_US=<modelo inglês>
NOISEBOT_TTS_DEFAULT_LANGUAGE=pt-BR
```

Compatibilidade:

- `NOISEBOT_PIPER_MODEL` continua aceito como modelo PT-BR legado;
- sem modelo inglês, o dashboard pode responder em inglês por texto, mas o
  robô não deve pronunciá-lo com voz portuguesa silenciosamente;
- o usuário recebe erro claro ou opção de resposta apenas no dashboard.

Primeiro corte: cada turno falado usa um único idioma. Resposta bilíngue com
troca de voz dentro do mesmo turno fica para uma fase posterior, pois exige
segmentos etiquetados e pacing entre providers.

## 12. Fases de Implementação

### V0 — Baseline reproduzível

- carga real de `medium/cpu/int8`;
- corpus dourado;
- runner offline;
- relatório comparativo `small/cpu/int8` vs `medium/cpu/int8`;
- amostras PT/EN/mistas e silêncio;
- deployment local configurado por `.env`, sem mudar firmware.

Gate: `medium` confirmado em CPU e relatório reproduzível com WER/CER, falsos
positivos, latência e uso de memória.

### V0.5 — Comparação CUDA

- preflight CUDA e diagnóstico de VRAM;
- carga de `large-v3/cuda/int8_float16`;
- mesmo corpus e mesmos prompts do V0;
- teste de coexistência com Ollama `gemma4:12b`;
- comparação de qualidade, latência e custo operacional.

Gate: promoção somente se o ganho for material e não houver OOM, unload
inesperado ou degradação inaceitável da LLM.

### V1 — STT contextual por idioma

- `SttRequestContext`;
- prompts PT/EN separados;
- idioma vindo da conversa ativa;
- telemetria de idioma e configuração;
- PT-BR como fallback compatível.

Gate: corpus inglês melhora sem regressão significativa no português.

### V2 — Confiança, segunda passagem e correção

- fallback alternativo sob baixa confiança;
- confirmação em empate;
- correção de transcrição no dashboard;
- histórico raw/effective;
- exclusão de turnos incertos do progresso.

Gate: erros graves caem sem duplicar latência em todos os turnos.

### V3 — LLM em inglês

- política dinâmica;
- guard por idioma;
- testes de PT, EN, bilíngue, código e tool calls;
- conversa de estudo responde em inglês.

Gate: inglês intencional não é substituído pelo fallback PT-BR e conversas
gerais continuam em português.

### V4 — TTS inglês

- `TtsRouter`;
- modelo inglês local configurável;
- cache/status por idioma;
- fallback explícito;
- teste real de resposta falada em inglês.

Gate: o NoiseBot fala inglês de modo inteligível, sem quebrar Piper PT-BR,
Playback v2, SAY, barge-in ou PCM16 rollback.

### V5 — Integração com estudos persistentes

- idioma por conversa;
- voz usa a conversa ativa;
- transcrição corrigida alimenta continuidade;
- progresso ignora turnos incertos;
- cenário de aula completo.

Gate: aula começa no dashboard, continua por voz em inglês, sobrevive a restart
e retoma do ponto correto.

## 13. Validação

Automatizada:

- unitários de seleção de idioma;
- prompts por idioma;
- comparação de hipóteses;
- guard de saída PT/EN;
- router TTS;
- histórico raw/effective;
- runner do corpus;
- testes atuais de server e bridge.

Hardware:

1. confirmar em status `model=medium`, `device=cpu`, `compute_type=int8`;
2. `voice-release-check` antes;
3. cinco turnos PT-BR;
4. cinco turnos em inglês;
5. troca explícita para explicação em português;
6. barge-in durante fala inglesa;
7. silêncio após TTS para no-echo;
8. Playback v2 sem drops;
9. Codec v2 saudável;
10. PCM16 rollback;
11. `voice-release-check` depois;
12. repetir o mesmo roteiro com `large-v3/cuda/int8_float16` em V0.5.

Frases mínimas de regressão:

- `Que horas são?`
- `Me conte uma curiosidade curta.`
- `What did we study yesterday?`
- `Let's practice the present perfect.`
- `I have lived here for five years.`
- `Explain my mistake in Portuguese.`
- `Stop speaking.`
- `Pare de falar.`

## 14. Rollback

- idioma ausente continua PT-BR;
- desativar fallback bilíngue por flag;
- manter `medium/cpu/int8` como rollback explícito da futura fase CUDA;
- usar somente Piper PT-BR;
- desativar voz inglesa mantendo texto inglês no dashboard;
- nenhuma fase remove PCM16 nem altera o firmware de áudio.

Flags sugeridas:

```text
NOISEBOT_STT_CONTEXT_LANGUAGE_ENABLED=0|1
NOISEBOT_STT_LOW_CONFIDENCE_RETRY_ENABLED=0|1
NOISEBOT_BILINGUAL_LLM_ENABLED=0|1
NOISEBOT_ENGLISH_TTS_ENABLED=0|1
```

## 15. Critério Global de Conclusão

A trilha bilíngue estará pronta quando:

- o baseline `medium/cpu/int8` estiver medido;
- status e logs mostrarem modelo/device/compute type sem expor segredos;
- a decisão entre CPU e CUDA estiver sustentada pelo mesmo corpus;
- o corpus comprovar melhoria do STT;
- português não regredir de forma relevante;
- inglês for transcrito, respondido e falado intencionalmente;
- transcrição incerta puder ser corrigida;
- histórico de estudo usar o texto corrigido;
- voz PT e EN tiverem fallback explícito;
- Voice Audio v2, no-echo, barge-in, Playback e Codec permanecerem verdes.
