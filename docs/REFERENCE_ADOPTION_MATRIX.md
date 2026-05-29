# Matriz de Adoção — XiaoZhi, StackChan e NoiseBot

Este documento fixa a análise consolidada antes das próximas fases. A regra é:
nenhuma feature nova entra no NoiseBot só porque existe no XiaoZhi ou StackChan.
Cada item precisa ter decisão, ordem, teste e rollback.

Referências locais analisadas:

- XiaoZhi:
  - `D:\Projetos\Xiaozhi-for-XiaoESP32S3-master\Source\xiaozhi-esp32-2.2.2\main\application.cc`
  - `D:\Projetos\Xiaozhi-for-XiaoESP32S3-master\Source\xiaozhi-esp32-2.2.2\main\audio\audio_service.cc`
  - `D:\Projetos\Xiaozhi-for-XiaoESP32S3-master\Source\xiaozhi-esp32-2.2.2\docs\websocket.md`
  - `D:\Projetos\Xiaozhi-for-XiaoESP32S3-master\Source\xiaozhi-esp32-2.2.2\main\audio\processors\afe_audio_processor.cc`
- StackChan:
  - `D:\Projetos\StackChan\firmware\main\main.cpp`
  - `D:\Projetos\StackChan\firmware\main\hal\board\cores3_audio_codec.cc`
  - `D:\Projetos\StackChan\firmware\main\stackchan\motion`
  - `D:\Projetos\StackChan\firmware\main\stackchan\modifiers`
  - `D:\Projetos\StackChan\firmware\main\apps`
- NoiseBot:
  - `docs/VOICE_PIPELINE.md`
  - `docs/BRIDGE_V2.md`
  - `docs/REFERENCE_ARCHITECTURES.md`
  - `bridge/noisebot_bridge/protocol.py`
  - `bridge/noisebot_bridge/runtime.py`
  - `components/infra/bridge_service.c`
  - `components/services/audio_service/audio_service.c`

## Decisão Executiva

O checkpoint atual fecha **Conversation Protocol v2 batch/half-duplex** e
**Opus experimental opt-in** como caminhos validados. O próximo avanço é
promover Opus de experimento manual para **modo negociado oficial com fallback
PCM16**, sem alterar wake word, VAD, follow-up, realtime ou AEC.

Motivo:

- XiaoZhi resolve estabilidade conversacional primeiro por estados explícitos,
  eventos de sessão e capacidades negociadas.
- StackChan agrega valor principalmente em expressividade, modifiers e produto,
  mas não é referência principal de protocolo de voz.
- NoiseBot já tem PCM16 estável, Opus opt-in validado em turno real e multi-turn,
  barge-in por wake word validado, no-echo validado e AEC de dispositivo
  bloqueado corretamente por falta de referência física.
- O maior risco remanescente é promover codec/fallback sem mexer no caminho de
  wake/VAD que voltou a ficar estável.

## Matriz

| Área | XiaoZhi | StackChan | NoiseBot atual | Decisão | Próxima ação | Teste automático |
| --- | --- | --- | --- | --- | --- | --- |
| Estados de conversa | `Application` centraliza `Idle`, `Connecting`, `Listening`, `Speaking`; liga voice processing só em `Listening`. | App loop e apps separados; não é motor conversacional principal. | `voice_controller` iniciado; bridge já tem eventos v2 e fake firmware para batch/half-duplex. | Adotar conceito do XiaoZhi, sem portar C++. | Evoluir FSM v2 no bridge antes de firmware. | Fake firmware já simula `wake -> listen -> speak -> idle`; falta reconexão e cancelamento explícito. |
| Wake em IDLE | Wake ativo em `Idle`; ao detectar abre canal e muda estado. | Não é referência principal de wake. | Wake local funciona; regressões recentes vieram de filtros agressivos; caso baseline já está em teste sem hardware. | Manter caminho atual e só mudar por contrato explícito. | Não mexer em threshold/VAD sem falha comprovada e teste. | Fake firmware valida wake/listen, wake sem áudio e áudio fora de sessão. |
| Escuta durante fala | Em `Speaking`, voice processing fica desligado em modo não realtime; wake pode abortar fala quando suportado. | Expressividade durante fala, não full-duplex agressivo. | Barge-in por wake foi validado em hardware com `barge-live`; VAD automático durante TTS causou falso positivo. | Manter half-duplex; realtime só futuro. | Não mexer em barge-in salvo regressão coberta por teste. | `barge-live` passou; teste de contrato trava `speaking -> wake abort -> no stale SAY`. |
| Follow-up | Estado/protocolo controla `listen start/stop`; modo realtime separado. | Não é referência principal. | Follow-up automático está em standby. | Manter standby até contrato v2 e testes. | Não reativar follow-up como side-effect. | Teste garante que `FOLLOWUP_ARM` não abre escuta quando feature off. |
| Protocolo de sessão | WebSocket com `hello`, `listen detect/start/stop`, `abort`, audio params e features. | Integra Xiaozhi como app; não substitui contrato. | TCP próprio já tem `HELLO`, `SESSION`, `SPEECH_CANCEL`, `SAY`, `VOICE_START/END`; testes cobrem schemas, sequência e falhas STT/TTS. | Evoluir protocolo atual, não trocar transporte agora. | Fechar reconexão e cancelamento explícito antes de firmware. | `test_protocol.py`, `test_voice_session.py` e `test_fake_firmware.py`. |
| Capacidades reais | `hello.audio_params` e features negociadas; AEC depende de modo. | CoreS3 informa codec/canais reais. | `HELLO` anuncia PCM por padrão; Opus liga por flag; `board_caps` bloqueia AEC sem referência. | Adotar negociação forte e não anunciar features falsas. | Promover Opus como capability negociada oficial mantendo `pcm16` fallback. | Teste do payload `HELLO`, parse de peer caps e fallback PCM16. |
| Codec | Opus 16 kHz mono, 60 ms, em WebSocket. | CoreS3 tem codec externo; não implica Opus para NoiseBot. | PCM16 16 kHz mono estável; Opus 60 ms passou em hardware com STT good, LLM/local intent e zero drops. | Promover Opus de opt-in experimental para modo oficial opt-in, ainda não padrão obrigatório. | Consolidar flag, fallback e status; rodar A/B curto de latência/CPU antes de tornar default. | `opus-live`, multi-turn Opus, `test_opus_codec.py`, fake firmware Opus e contrato PCM16 default. |
| AFE/VAD | Voice processor ligado só em `Listening`; AFE separado de wake. | CoreS3 hardware ajuda, mas não é igual ao NoiseBot. | AFE VC opt-in passou; RAW teve melhor confiança STT. | Manter RAW padrão, AFE opt-in. | Nenhuma promoção de AFE sem A/B maior. | Teste de config garante AFE off por padrão. |
| AEC | Suportado quando há referência/canais; pode alternar device/server AEC. | CoreS3 usa ES7210/AW88298 com referência limpa. | INMP441 + MAX98357A não fornece referência física; `aec-live` retornou `aec_blocked_no_reference=true` e `ESP_ERR_NOT_SUPPORTED`. | Não promover AEC de dispositivo neste hardware atual; server-side AEC fica futuro até existir referência de playback. | Manter endpoint diagnóstico e recomendação explícita; não usar AEC como bloqueio para Opus. | `aec-live` classifica `promotable=false`; teste aceita HTTP 500 diagnóstico sem traceback. |
| Transporte | WebSocket/MQTT com hello e binário Opus. | Pode iniciar XiaoZhi como app. | TCP local custom, offline-first. | Não migrar transporte antes do contrato. | Bridge v2 primeiro sobre TCP atual. | Fake firmware byte-compatível com framing atual. |
| MCP/tools | MCP como canal de tools e device capabilities. | Tools locais de robô e apps. | Intents locais e device commands já existem parcialmente. | Adotar tools com schemas, mas depois do protocolo v2. | Formalizar tools locais determinísticas antes de LLM tools. | Testes de schema, limites e dispatcher local. |
| Expressividade | Display/status acompanha estado conversacional. | Melhor referência: avatar, modifiers, blink, breath, speaking, touch. | Conductor, expression, idle, overlays e touch já existem. | Adotar padrões de modifiers do StackChan, sem Mooncake. | Mapear modifiers para serviços existentes. | Testes unitários de seleção de ação/expressão quando possível. |
| Movimento/servos | Não é referência principal. | Possui motion/servo expressivos. | Motion safety é regra de veto. | Aproveitar conceito, não código direto, até safety estar verde. | Nenhum movimento novo sem `motion_safety`. | Testes de veto de safety antes de qualquer port. |
| UI/setup | Status visual claro por estado. | Setup/app center/diagnóstico de produto são fortes. | Dashboard local e overlays existem, mas dev-heavy. | Adotar diagnóstico de produto aos poucos. | Melhorar dashboard depois do protocolo v2. | Teste API local não pode derrubar voz. |
| Áudio de feedback | Sons curtos para transição de listening/speaking. | Assets/sfx e toasts. | `PODE FALAR`, toasts, overlays. | Manter simples; não usar som que atrapalhe STT. | Só ajustar feedback se houver métrica de não interferência. | Replay de wake sem fala e wake com fala. |
| Erros | Erros nomeados, network callbacks, abort. | Apps isolam telas e setup. | Já há `SESSION_ERROR`, `FOLLOWUP_CANCEL`, watchdog. | Expandir taxonomia de erro no protocolo v2. | Erros sem silêncio absoluto e sem loop. | Testes STT vazio, LLM falha, TTS falha, timeout. |
| Replay/testes | Não é foco do firmware, mas arquitetura permite simulação. | Não é referência principal. | Há testes de bridge, voice_session, protocolo, replay, fake firmware e harnesses live para Opus/barge/no-echo/AEC; server está com 99 testes verdes. | Virar requisito antes de firmware novo. | Expandir para reconexão, fixtures WAV reais e cancelamento explícito. | CI/pytest com fake firmware, sequências ruins e probes live quando houver hardware. |
| Dependências | C++ amplo, esp-sr, esp-audio-codec, WebSocket, MCP. | Mooncake UI, M5Stack, codecs CoreS3, app mobile. | C17 por regra, C++ apenas display. | Não importar dependências inteiras sem necessidade/licença. | Portar conceitos, não árvores de código. | Revisão de licença por item antes de dependência nova. |

## Ordem Correta a Partir de Agora

1. **Protocol v2 em testes automáticos** — iniciado/concluído para o contrato
   batch atual.
   - Definir payloads `HELLO`, `SESSION`, `LISTEN_START`, `LISTEN_STOP`,
     `SPEAK_START`, `SPEAK_STOP`, `ABORT_SPEAKING`.
   - Garantir que `pcm16` continua padrão.
   - Garantir que `opus`, `realtime`, `aec` e `followup` não são anunciados como
     ativos quando estão desligados.

2. **Fake firmware / replay de protocolo** — iniciado.
   - Simular firmware enviando `HELLO`, `VOICE_START`, `AUDIO_CHUNK`,
     `VOICE_END`.
   - Simular frames corrompidos, áudio fora de sessão, sessões vazias e falhas
     STT/TTS.
   - Próximo: simular reconexão e cancelamento explícito de fala.

3. **FSM de conversa no bridge**
   - Estados mínimos: `idle`, `listening`, `transcribing`, `thinking`,
     `speaking`, `interrupted`, `error`.
   - Invariantes:
     - só envia `SAY` em `speaking`;
     - só aceita áudio de usuário em `listening`;
     - `turn_id` monotônico descarta saída antiga;
     - erro terminal sempre volta ao baseline.

4. **Firmware: ajuste mínimo para refletir contrato**
   - Somente depois dos testes do bridge.
   - Não alterar wake/VAD que já funciona.
   - Adicionar campos de capacidade se necessário.

5. **Opus Fase 6**
   - Implementado e validado como modo experimental opt-in.
   - `pcm16` permanece fallback e padrão.
   - Frame de 60 ms compatível com XiaoZhi.
   - Firmware validado com worker persistente, fila PSRAM e envio/dreno sem
     drops em teste real.
   - Próximo passo: tornar Opus capability oficial opt-in, com fallback PCM16
     explícito e sem tocar wake/VAD.

6. **Tools/MCP**
   - Primeiro tools locais determinísticas.
   - Depois ponte compatível com MCP.
   - Comandos físicos sempre passam pelas camadas do NoiseBot e safety.

7. **AEC/realtime**
   - AEC de dispositivo fica em standby por falta de referência limpa.
   - Server-side AEC continua futuro e depende de referência/timestamps de
     playback.
   - Realtime/follow-up não deve ser reativado como efeito colateral de Opus.

## Itens Explicitamente Bloqueados

- Promover AFE ou Opus para padrão obrigatório sem A/B maior e sessão longa
  ponta a ponta.
- Habilitar AEC `MR`/`MMR` no hardware INMP441 + MAX98357A atual.
- Reativar follow-up automático sem teste de protocolo.
- Trocar TCP por WebSocket antes do contrato v2 estar estável.
- Importar Mooncake UI, app center, `esp_codec_dev` ou árvore CoreS3 inteira.
- Criar novos movimentos expressivos sem `motion_safety` verde.

## Critérios Para Avançar De Fase

Antes de qualquer fase nova:

1. A matriz precisa indicar `Decisão` e `Próxima ação`.
2. O item precisa estar no roadmap.
3. Deve existir teste automático quando o comportamento puder ser simulado.
4. Se tocar firmware, o escopo deve dizer quais arquivos podem mudar.
5. Deve existir rollback claro.

## Próximo Item Recomendado

Promover **Opus capability oficial opt-in com fallback PCM16**, sem tocar no
firmware de wake/VAD:

- deixar status/HELLO/metrics coerentes entre firmware e server;
- garantir fallback PCM16 quando Opus não é confirmado;
- manter `opus-live` e testes automáticos como gate;
- documentar que AEC/realtime/follow-up seguem em standby.

Essa etapa só toca codec/negociação. Qualquer mudança em wake, VAD, AEC,
follow-up ou realtime fica fora do escopo.
