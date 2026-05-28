# Arquiteturas de Referencia — StackChan e XiaoZhi

Este documento registra o que o NoiseBot deve aproveitar das bases StackChan e XiaoZhi sem copiar o firmware inteiro. A regra geral e: importar arquitetura, contratos e criterios de produto; portar codigo apenas quando houver necessidade clara, compatibilidade com a licenca e aderencia as camadas do NoiseBot.

A matriz operacional de adoção fica em `docs/REFERENCE_ADOPTION_MATRIX.md`.
Antes de iniciar uma nova fase, a decisão deve estar registrada ali com teste e
rollback claros.

## Decisao Principal

Nao reiniciar o NoiseBot em cima do firmware StackChan/XiaoZhi.

Motivos:

- O StackChan e fortemente acoplado ao CoreS3, M5Stack, Mooncake UI, camera, app mobile, server proprio e Xiaozhi.
- O XiaoZhi usa uma arquitetura C++ e um motor conversacional proprio que nao respeita diretamente as camadas do NoiseBot.
- O NoiseBot ja tem ESP-IDF C17, event bus, motion safety, behavior engine, LTM, touch afetivo, display, SD e bridge validados em hardware real.
- Substituir a base inteira aumentaria risco de regressao em safety, audio, memoria e produto offline-first.

O caminho escolhido e evolucao incremental por ilhas de valor.

## StackChan — O Que Aproveitar

### Produto e Expressividade

O StackChan separa bem o motor conversacional da camada fisica/expressiva do robo. O motor emite estados e tools; a camada StackChan traduz isso em avatar, boca, movimento, LEDs e toque.

Adotar no NoiseBot:

- estados visuais derivados do motor conversacional;
- overlay de fala com boca/expressao/gaze/micro-movimento;
- modifiers/overlays independentes para blink, breath, idle, speaking, touch e humor;
- toque como canal afetivo, nao como gatilho obrigatorio de escuta;
- setup/diagnostico com experiencia de produto, nao apenas logs seriais.

Nao adotar diretamente:

- Mooncake UI;
- app center completo neste momento;
- server social/remoto como dependencia;
- acoplamento direto de UI, servos e rede sem event bus.

### Tools do Robo

StackChan expoe tools locais via MCP, como controle de cabeca, LEDs e lembretes.

Adotar no NoiseBot:

- tools com nomes estaveis e descricoes ricas;
- limites de seguranca embutidos no schema;
- dispatcher local antes de LLM;
- lembretes locais como feature de companion;
- separacao entre comando local fisico e resposta textual local.

Exemplos desejados:

- `noisebot.robot.get_status`
- `noisebot.robot.set_gaze`
- `noisebot.robot.set_expression`
- `noisebot.robot.set_led_mood`
- `noisebot.robot.create_reminder`
- `noisebot.robot.stop_reminder`

Todo comando fisico continua subordinado as camadas do NoiseBot, especialmente `motion_safety`.

## XiaoZhi — O Que Aproveitar

### Motor Conversacional

O XiaoZhi e a referencia principal para conversa: wake local, estados explicitos, canal de audio sob demanda, Opus/WebSocket, AFE/VAD separado e tools via MCP.

Adotar no NoiseBot:

- estados explicitos: `connecting`, `idle`, `listening`, `transcribing`, `thinking`, `speaking`, `error`;
- protocolo com `hello`, `wake/detect`, `listen/start`, `listen/stop`, `tts/start`, `tts/stop`;
- audio compacto em Opus para uma futura ponte de produto;
- handshake de capacidades;
- erros nomeados e sem silencio absoluto;
- replay offline para testar STT/routing sem acordar o robo.
- politica unica de pipeline de voz: wake ativo em estados permitidos, voice
  processor ativo apenas durante `listening`, e `speaking` sem captura de voz
  concorrente exceto quando houver modo realtime explicitamente medido.
- um controlador de voz dedicado, no estilo da `Application` do XiaoZhi, para
  que wake/listening/speaking/follow-up nao fiquem espalhados por callbacks de
  boot.

Nao adotar diretamente:

- dependencia de cloud especifica;
- inicializacao monolitica;
- comandos que passam ao redor do event bus;
- presuncao de hardware CoreS3/codec ES7210.

### Audio, Codec e AEC

XiaoZhi/StackChan nao tratam AEC como recurso generico do ESP32-S3. O AEC no
dispositivo depende da placa: `AudioCodec` informa se ha `input_reference()` e
quantos canais entram no AFE. No CoreS3/StackChan, o codec ES7210/AW88298
oferece caminho full-duplex com canal limpo de referencia do speaker. No
NoiseBot atual, INMP441 + MAX98357A nao fornece esse canal.

Adotar no NoiseBot:

- descritor de capacidades da placa (`nb_board_caps_t`) como fonte de verdade;
- device AEC condicionado a `supports_device_aec`;
- `input_format` do ESP-SR derivado das capacidades reais, nao de tentativa;
- endpoint de diagnostico explicando quando AEC foi bloqueado por falta de
  referencia fisica.

Nao adotar diretamente:

- habilitar `MR`/`MMR` sem canal `R` real;
- importar `esp_codec_dev`/`esp_audio_codec` antes de existir codec externo;
- usar AEC para mascarar problema de turn-taking, wake rearm ou contrato de
  sessao.

## Mapa de Adoção

### Curto Prazo

- Documentar e estabilizar o bridge atual.
- Formalizar intents e device commands.
- Adicionar diagnostico de sessao e replay.
- Criar overlays de listening/speaking/error no NoiseBot.
- Centralizar capacidades reais de hardware e bloquear features incompatíveis
  por contrato, nao por tentativa em runtime.
- Iniciar `voice_controller` como dono incremental das regras de turn-taking no
  firmware.

### Medio Prazo

- Definir Conversation Protocol v2 mantendo compatibilidade com o protocolo atual.
- Implementar tools v2 com schemas e limites.
- Adicionar lembretes locais.
- Melhorar setup/diagnostico web.

### Longo Prazo

- Avaliar Opus/WebSocket.
- Avaliar service remoto/produto.
- Avaliar AEC/dual mic apenas depois do caminho com 1 mic estar mensurado.
- Avaliar app/hub externo se houver necessidade real de produto.

## Regras de Portabilidade

- Toda ideia importada precisa de etapa propria no roadmap.
- Toda mudanca em firmware deve preservar offline-first.
- Nenhum movimento novo ignora `motion_safety`.
- Nenhum codigo externo entra sem revisao de licenca e necessidade tecnica.
- Cada port deve ter criterio de aceitacao e rollback claro.
