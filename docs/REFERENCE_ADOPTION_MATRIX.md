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

O próximo avanço continua sendo **Conversation Protocol v2 com testes
automáticos**, não Opus, não AEC e não troca do pipeline de áudio. A primeira
camada dessa decisão já foi implementada: o bridge agora tem testes de
capabilities, contrato de sessão e fake firmware byte-compatível para o fluxo
batch/half-duplex atual.

Motivo:

- XiaoZhi resolve estabilidade conversacional primeiro por estados explícitos,
  eventos de sessão e capacidades negociadas.
- StackChan agrega valor principalmente em expressividade, modifiers e produto,
  mas não é referência principal de protocolo de voz.
- NoiseBot já tem RAW estável, AFE opt-in saudável e AEC bloqueado corretamente
  por falta de referência física.
- O maior risco remanescente é contrato de conversa, não codec.

## Matriz

| Área | XiaoZhi | StackChan | NoiseBot atual | Decisão | Próxima ação | Teste automático |
| --- | --- | --- | --- | --- | --- | --- |
| Estados de conversa | `Application` centraliza `Idle`, `Connecting`, `Listening`, `Speaking`; liga voice processing só em `Listening`. | App loop e apps separados; não é motor conversacional principal. | `voice_controller` iniciado; bridge já tem eventos v2 e fake firmware para batch/half-duplex. | Adotar conceito do XiaoZhi, sem portar C++. | Evoluir FSM v2 no bridge antes de firmware. | Fake firmware já simula `wake -> listen -> speak -> idle`; falta reconexão e cancelamento explícito. |
| Wake em IDLE | Wake ativo em `Idle`; ao detectar abre canal e muda estado. | Não é referência principal de wake. | Wake local funciona; regressões recentes vieram de filtros agressivos; caso baseline já está em teste sem hardware. | Manter caminho atual e só mudar por contrato explícito. | Não mexer em threshold/VAD sem falha comprovada e teste. | Fake firmware valida wake/listen, wake sem áudio e áudio fora de sessão. |
| Escuta durante fala | Em `Speaking`, voice processing fica desligado em modo não realtime; wake pode abortar fala quando suportado. | Expressividade durante fala, não full-duplex agressivo. | Barge-in por wake funciona de forma básica; VAD automático durante TTS causou falso positivo. | Manter half-duplex; realtime só futuro. | Documentar `speaking` como sem captura concorrente por padrão. | Teste `speaking -> wake abort -> no stale SAY`. |
| Follow-up | Estado/protocolo controla `listen start/stop`; modo realtime separado. | Não é referência principal. | Follow-up automático está em standby. | Manter standby até contrato v2 e testes. | Não reativar follow-up como side-effect. | Teste garante que `FOLLOWUP_ARM` não abre escuta quando feature off. |
| Protocolo de sessão | WebSocket com `hello`, `listen detect/start/stop`, `abort`, audio params e features. | Integra Xiaozhi como app; não substitui contrato. | TCP próprio já tem `HELLO`, `SESSION`, `SPEECH_CANCEL`, `SAY`, `VOICE_START/END`; testes cobrem schemas, sequência e falhas STT/TTS. | Evoluir protocolo atual, não trocar transporte agora. | Fechar reconexão e cancelamento explícito antes de firmware. | `test_protocol.py`, `test_voice_session.py` e `test_fake_firmware.py`. |
| Capacidades reais | `hello.audio_params` e features negociadas; AEC depende de modo. | CoreS3 informa codec/canais reais. | `HELLO` anuncia PCM e features; `board_caps` bloqueia AEC sem referência. | Adotar negociação forte e não anunciar features falsas. | Expandir `HELLO` com `aec_supported=false`, `realtime=false`, `opus=false`, `afe_opt_in=true`. | Teste do payload `HELLO` e parse de peer caps. |
| Codec | Opus 16 kHz mono, 60 ms, em WebSocket. | CoreS3 tem codec externo; não implica Opus para NoiseBot. | PCM16 16 kHz mono estável. | Opus fica Fase 6, opcional e com fallback. | Só iniciar Opus depois dos testes do protocolo v2. | Teste deve garantir `pcm16` default e `opus` off. |
| AFE/VAD | Voice processor ligado só em `Listening`; AFE separado de wake. | CoreS3 hardware ajuda, mas não é igual ao NoiseBot. | AFE VC opt-in passou; RAW teve melhor confiança STT. | Manter RAW padrão, AFE opt-in. | Nenhuma promoção de AFE sem A/B maior. | Teste de config garante AFE off por padrão. |
| AEC | Suportado quando há referência/canais; pode alternar device/server AEC. | CoreS3 usa ES7210/AW88298 com referência limpa. | INMP441 + MAX98357A não fornece referência física; AEC bloqueado. | Não implementar AEC de dispositivo neste hardware atual. | Manter endpoint diagnóstico e erro explicativo. | Teste de status deve expor `aec_supported=false`. |
| Transporte | WebSocket/MQTT com hello e binário Opus. | Pode iniciar XiaoZhi como app. | TCP local custom, offline-first. | Não migrar transporte antes do contrato. | Bridge v2 primeiro sobre TCP atual. | Fake firmware byte-compatível com framing atual. |
| MCP/tools | MCP como canal de tools e device capabilities. | Tools locais de robô e apps. | Intents locais e device commands já existem parcialmente. | Adotar tools com schemas, mas depois do protocolo v2. | Formalizar tools locais determinísticas antes de LLM tools. | Testes de schema, limites e dispatcher local. |
| Expressividade | Display/status acompanha estado conversacional. | Melhor referência: avatar, modifiers, blink, breath, speaking, touch. | Conductor, expression, idle, overlays e touch já existem. | Adotar padrões de modifiers do StackChan, sem Mooncake. | Mapear modifiers para serviços existentes. | Testes unitários de seleção de ação/expressão quando possível. |
| Movimento/servos | Não é referência principal. | Possui motion/servo expressivos. | Motion safety é regra de veto. | Aproveitar conceito, não código direto, até safety estar verde. | Nenhum movimento novo sem `motion_safety`. | Testes de veto de safety antes de qualquer port. |
| UI/setup | Status visual claro por estado. | Setup/app center/diagnóstico de produto são fortes. | Dashboard local e overlays existem, mas dev-heavy. | Adotar diagnóstico de produto aos poucos. | Melhorar dashboard depois do protocolo v2. | Teste API local não pode derrubar voz. |
| Áudio de feedback | Sons curtos para transição de listening/speaking. | Assets/sfx e toasts. | `PODE FALAR`, toasts, overlays. | Manter simples; não usar som que atrapalhe STT. | Só ajustar feedback se houver métrica de não interferência. | Replay de wake sem fala e wake com fala. |
| Erros | Erros nomeados, network callbacks, abort. | Apps isolam telas e setup. | Já há `SESSION_ERROR`, `FOLLOWUP_CANCEL`, watchdog. | Expandir taxonomia de erro no protocolo v2. | Erros sem silêncio absoluto e sem loop. | Testes STT vazio, LLM falha, TTS falha, timeout. |
| Replay/testes | Não é foco do firmware, mas arquitetura permite simulação. | Não é referência principal. | Há testes de bridge, voice_session, protocolo, replay e fake firmware; suíte atual: 135 testes verdes. | Virar requisito antes de firmware novo. | Expandir para reconexão, fixtures WAV reais e cancelamento explícito. | CI/pytest com fake firmware e sequências ruins. |
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
   - Implementado como modo experimental opt-in.
   - `pcm16` permanece fallback e padrão.
   - Frame de 60 ms compatível com XiaoZhi.
   - Firmware validado com worker persistente, fila PSRAM e envio/dreno sem
     drops em teste real.
   - Só promover como padrão após sessão longa com STT/LLM/TTS validada no
     server e sem regressão perceptível.

6. **Tools/MCP**
   - Primeiro tools locais determinísticas.
   - Depois ponte compatível com MCP.
   - Comandos físicos sempre passam pelas camadas do NoiseBot e safety.

7. **AEC/realtime**
   - Continua futuro.
   - Só revisitar com hardware que forneça referência limpa de playback ou com
     server-side AEC validado.

## Itens Explicitamente Bloqueados

- Promover AFE ou Opus para padrão sem A/B maior e sessão longa ponta a ponta.
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

Expandir os testes automáticos de **Conversation Protocol v2** no bridge para os
pontos ainda não cobertos:

- reconexão TCP/UART sem sessão pendente;
- cancelamento explícito de fala antes de qualquer novo ajuste no firmware;
- fixtures WAV reais boas/ruins entrando no replay;
- long-run automatizado curto para detectar estado fantasma.

Essa etapa não exige flash, não exige falar com o robô e reduz o risco de novas
rodadas manuais de wake/listening.
