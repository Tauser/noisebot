# Roadmap Canônico da Migração Dual-MCU

**Status:** ativo  
**Atualizado em:** 2026-06-20  
**Escopo:** migração do firmware monolítico para Waveshare main-controller +
Freenove head-controller  
**Autoridade:** este documento define ordem, IDs, dependências e gates do
programa DM. `docs/ROADMAP.md` define prioridade entre programas do produto;
`docs/DUAL_MCU_ARCHITECTURE_PLAN.md` define a arquitetura-alvo.

## 1. Por que este documento existe

A primeira sequência DM tratou “display remoto funcional” como equivalente a
“migração visual concluída”. Isso permitiu abrir câmera e storage enquanto
faces, animações, estados, texto, status rail, preview e parte do render ainda
tinham autoridade dividida entre os MCUs.

Este roadmap corrige esse problema:

- uma fase só termina quando a responsabilidade inteira muda de dono;
- provas técnicas parciais são preservadas como subfases, nunca promovidas a
  conclusão da fase;
- nenhuma fase posterior absorve silenciosamente dívida de uma fase anterior;
- agentes não podem inventar novas fases ou subfases durante a implementação;
- cada corte mantém rollback e um estado executável conhecido.

## 2. Regras de governança para agentes

Estas regras são obrigatórias para qualquer trabalho DM.

1. Só existem os IDs `DM*` e `DMM.*` registrados na seção 8.
2. É proibido criar `DM7`, `DM2.16`, `DMM.14`, sufixos improvisados ou
   “etapas intermediárias” não registradas.
3. Se um trabalho necessário não couber em um ID existente, o agente deve
   parar e propor alteração deste documento. A alteração precisa declarar
   motivo, dependências, gate e impacto na ordem.
4. Uma subfase pode mudar para `FEITO` somente com todos os critérios de saída
   registrados e evidência no documento de bring-up ou migração correspondente.
5. Build limpo prova compilação; não prova hardware, paridade visual,
   recuperação, latência nem comportamento de produto.
6. Log de apenas um MCU não prova fluxo ponta a ponta.
7. ACK de frame prova recepção do protocolo, não aplicação de domínio.
8. Spike não altera a ordem do programa. Código de spike fica desabilitado por
   padrão e não autoriza a fase seguinte.
9. Não remover fallback ou código legado antes do gate de corte que o substitui.
10. Commits devem ser pequenos e associados a um único ID DM.
11. Ao iniciar trabalho, registrar no plano do agente o ID exato. Ao terminar,
    informar os gates executados e os ainda pendentes.
12. `CLAUDE.md` continua sendo a autoridade máxima do repositório.

## 3. Princípios de escalabilidade e robustez

- O main é a única autoridade de comportamento, FSM, emoção, persona, tempo,
  safety, áudio, servos, LEDs e touch corporal.
- O head é executor multimídia: apresenta estado visual, captura câmera,
  processa touch de tela e executa storage.
- O main envia intenção semântica e snapshots completos; nunca envia frames de
  display prontos.
- O head pode interpolar e animar apresentação, mas não inventa emoção,
  transição comportamental ou ação.
- Cada domínio tem contrato versionado, capability própria, fila limitada,
  backpressure explícito e telemetria.
- Somente a task proprietária do enlace chama `nb_link_engine_tick()`,
  `nb_link_engine_on_frame()` e `nb_link_engine_send()`. Outros componentes
  usam filas.
- Estado persistente e estado transitório são separados. Reconexão restaura o
  snapshot persistente e descarta transientes vencidos.
- `IDLE` permanece o baseline: `NEUTRAL`, gaze central, postura central, LED
  idle e nenhum overlay transitório.
- Nenhum framebuffer de display ou câmera usa SRAM.
- Falha do head nunca bloqueia safety, áudio ou comportamento da main.

## 4. Propriedade por domínio

| Domínio | Estado atual durante migração | Dono final | Interface entre MCUs |
| --- | --- | --- | --- |
| FSM, emoção, persona | Main | Main | Snapshot visual derivado |
| Expressão escolhida | Main | Main | `VISUAL_SCENE` |
| Geometria da face | Main e head, ainda divergentes | Head | Parâmetros semânticos |
| Interpolação visual e blink | Principalmente main | Head | Timeline/targets |
| Gaze visual | Main calcula e ambos desenham parcialmente | Main decide; head anima | Target, velocidade e geração |
| Estados visuais | Main | Main decide; head apresenta | Snapshot de estado |
| Overlays, texto e status | Main; oito flags remotas | Head | Slots/objetos semânticos |
| Assets visuais | Main e código do head | Head | IDs versionados; bulk futuro |
| ST7789/LovyanGFX | Ambos durante fallback | Head | Nenhuma API gráfica cruza o link |
| Touchscreen | Não instalado | Head mede; main decide | Evento cru normalizado |
| OV2640/DVP | HAL novo no head; produto ainda usa legado | Head | Comando, evento e BULK |
| Preview/bbox | Main legado | Head | Estado de preview e bbox |
| Análise de visão | Server/bridge | Server/bridge | JPEG sob demanda |
| microSD/FATFS | Main legado | Head | STORAGE + BULK |
| Áudio I2S | Main | Main | Assets remotos pré-bufferizados |
| Motion/safety | Main | Main | Proibido no head |

### 4.1 Situação real da Waveshare

O firmware de produção ainda não concluiu a troca de placa:

- `nb_hw_config.h` continua sendo o mapa monolítico da Freenove;
- `nb_hw_config_main.h` contém a proposta da Waveshare, ainda pendente de gate
  integrado e seleção explícita pelo build normal;
- DM1 usou a Waveshare em perfil mínimo de bancada para validar o enlace;
- áudio, touch corporal, LEDs e servos ficaram desconectados durante DM1;
- portanto, link aprovado não significa main-controller migrada.

A migração da Waveshare é a trilha **DMM** da seção 8. Ela é independente da
paridade visual de DM2, mas ambas precisam terminar antes do cutover de produto.

## 5. Conexões físicas aprovadas

Não alterar estes sinais sem atualizar `docs/GPIO_DUAL_MCU.md`, o HAL das duas
placas e este roadmap.

### 5.1 Enlace inter-MCU

| Sinal | Main Waveshare | Head Freenove | Direção |
| --- | --- | --- | --- |
| `LINK_SCLK` | GPIO12 / SPI2 | GPIO41 / SPI3 | main → head |
| `LINK_MOSI` | GPIO11 / SPI2 | GPIO42 / SPI3 | main → head |
| `LINK_MISO` | GPIO13 / SPI2 | GPIO14 / SPI3 | head → main |
| `LINK_CS` | GPIO10 / SPI2 | GPIO1 / SPI3 | main → head |
| `HEAD_IRQ` | GPIO14 | GPIO2 | head → main |
| `HEAD_RESET` | GPIO8 | EN | main → head |
| GND | GND | GND | comum |

Baseline aprovado: 10 MHz. Promoção para 20 MHz exige gate próprio de
integridade; 40 MHz do ST7789 é outro barramento e não altera o link.

### 5.2 Head multimídia

| Recurso | Barramento/pinos | Regra |
| --- | --- | --- |
| ST7789 | SPI2: SCLK 47, MOSI 21, DC 45, RST 3, CS GND | 40 MHz aprovado; 20 MHz rollback |
| OV2640 | DVP fixo GPIO4–18 conforme `nb_hw_config_head.h` | Nunca remapear |
| SCCB/I2C | SDA 4, SCL 5 | Compartilhável somente após checar endereço |
| microSD | SDMMC 1-bit: CMD 38, CLK 39, DATA0 40 | Único SD do produto |
| Console | UART0 GPIO43/44 | Não reutilizar |

### 5.3 Main física

Áudio, servo, LEDs e touch corporal pertencem à Waveshare conforme
`docs/GPIO_DUAL_MCU.md`. GPIO47/48 da N32R16V são domínio de 1,8 V e não podem
ser usados como lógica 3,3 V. GPIO35/36/37 são NC.

| Recurso | Waveshare | Interface/regra |
| --- | --- | --- |
| Touch corporal | GPIO2 | touch nativo; calibração refeita na nova montagem |
| I2C sensores | SDA 4, SCL 5 | I2C0, 400 kHz, pull-ups externos |
| Monitor 5 V | GPIO7 | ADC com divisor externo; bancada atual validada com 100k/100k |
| Servo | TX 17, RX 18 | UART1, 1 Mbps, FE-TTLinker; torque desabilitado |
| LEDs externos | GPIO21 | RMT, 2× WS2812, level shift para 5 V |
| LED onboard | GPIO38 | WS2812 da Waveshare, separado dos LEDs do robô |
| Microfone | SD 39 | INMP441, I2S0 RX |
| Áudio clocks | BCLK 40, WS 41 | compartilhados RX/TX |
| Speaker | DIN 42 | MAX98357A, I2S0 TX |
| USB nativo | GPIO19/20 | reservado; servo não pode reutilizar |
| Console | GPIO43/44 | CH343/UART0, reservado |

## 6. Arquitetura dos contratos

O contrato compartilhado vive em
`firmware/shared/components/nb_inter_mcu_protocol`.

Cada domínio novo deve possuir:

- versão explícita;
- tamanho estático validado;
- campos reservados zerados;
- capability independente;
- request/generation ID;
- resposta de domínio quando aplicável;
- política de idempotência;
- timeout e resultado ambíguo definidos;
- teste host de payload válido, inválido, reservado, versão e wrap;
- fila de entrada na task proprietária do link;
- telemetria de aceito, rejeitado, substituído, backpressure e timeout.

### 6.1 Contrato visual alvo

O snapshot visual alvo não será uma enumeração crescente de casos especiais.
Ele será composto por blocos versionados:

- identidade do snapshot: `generation`, versão e timestamp monotônico do main;
- estado de alto nível: `BOOT_UP`, `IDLE`, `ATTENTIVE`, `RESPONDING`,
  `TOUCH_REACTING`, `SLEEPING`, `ERROR`, `SAFE_MODE`, `MEDITATION`,
  `SILENT_COMPANY`, `MAINTENANCE`;
- face: expressão base, parâmetros assimétricos, abertura, squint, tilt e cor;
- gaze: target X/Y, velocidade, hold e política de retorno;
- animação: ID da animação, duração, fase e flags de interrupção;
- overlays transitórios: listening, speaking, heart, blush, alerta, timer;
- status persistente: mic, câmera, speaker, bridge, Wi-Fi, SD, energia e safety;
- conteúdo textual: ID, severidade, duração e texto limitado;
- composição: prioridade, z-order, timeout e owner lógico;
- preview de câmera: ativo, bbox e modo, adicionado apenas em DM4.

O payload de controle permanece pequeno. Blocos maiores, texto extenso e assets
usam mensagens auxiliares ou BULK; não se aumenta indefinidamente uma struct.

## 7. Máquina de promoção

Status permitidos:

| Status | Significado |
| --- | --- |
| `BLOQUEADO` | Dependência anterior ainda não fechada |
| `PRONTO` | Dependências e pré-condições atendidas |
| `EM ANDAMENTO` | Um único ID está sendo implementado |
| `EM VALIDAÇÃO` | Código pronto; gates ainda incompletos |
| `PAUSADO` | Trabalho preservado, mas fora da ordem ativa |
| `FEITO` | Todos os gates e documentação aprovados |

Regras de promoção:

- somente uma subfase de implementação por domínio fica `EM ANDAMENTO`;
- documentação pode acompanhar o mesmo ID, não cria um ID separado;
- a próxima subfase só vira `PRONTO` após a anterior ficar `FEITO`;
- exceções exigem alteração explícita da tabela da seção 8;
- falha encontrada em gate reabre a própria subfase, não cria uma nova.

## 8. Registro fechado de fases e subfases

Esta tabela é o inventário completo. IDs não listados não existem.

### DM0 — Fundação e contrato comum

**Objetivo:** separar builds e criar protocolo compartilhado sem mudar o
produto.

| ID | Entrega | Status |
| --- | --- | --- |
| DM0.1 | Estrutura main/head/shared e builds independentes | `FEITO` |
| DM0.2 | Framing, CRC, sequence, canais e testes host | `FEITO` |
| DM0.3 | Documentação de autoridade, hardware e rollback | `FEITO` |

**Gate da fase:** dois builds, protocolo host verde e nenhum GPIO ativado por
engano.

### DM1 — Enlace físico e recuperação

**Objetivo:** tornar o link uma infraestrutura confiável antes de transportar
domínios de produto.

| ID | Entrega | Status |
| --- | --- | --- |
| DM1.1 | SPI master/slave, IRQ e reset | `FEITO` |
| DM1.2 | HELLO, boot ID, snapshot, heartbeat e capabilities | `FEITO` |
| DM1.3 | ACK, retry, idempotência, prioridade e backpressure | `FEITO` |
| DM1.4 | Falhas, reboot isolado, head ausente e recuperação | `FEITO` |
| DM1.5 | Soak de 8 h a 10 MHz e telemetria | `FEITO` |

**Gate da fase:** `docs/DM1_BRINGUP.md` aprovado. O link não pode interferir
em safety ou áudio.

### DMM — Migração funcional para a Waveshare main-controller

**Objetivo:** transferir para a Waveshare todos os recursos que pertencem ao
corpo e à autoridade principal do robô, preservando comportamento e safety.

DMM não move display, câmera, touchscreen ou SD. Esses recursos pertencem ao
head e seguem DM2–DM5. DMM também não autoriza movimento real antes do gate de
`motion_safety`.

| ID | Entrega | Status |
| --- | --- | --- |
| DMM.1 | Inventário elétrico, variante da placa e matriz de recabeamento | `FEITO` |
| DMM.2 | Perfil de placa e seleção segura do HAL da Waveshare | `FEITO` |
| DMM.3 | Alimentação, GND, ADC de 5 V e brownout | `EM ANDAMENTO` |
| DMM.4 | Áudio físico INMP441/MAX98357A | `BLOQUEADO` |
| DMM.5 | Voice Audio v2, wake, VAD e playback na Waveshare | `BLOQUEADO` |
| DMM.6 | Touch corporal e calibração na Waveshare | `BLOQUEADO` |
| DMM.7 | LEDs externos e LED onboard | `BLOQUEADO` |
| DMM.8 | UART/TTLinker e servos com torque bloqueado | `BLOQUEADO` |
| DMM.9 | Motion safety e liberação controlada dos servos | `BLOQUEADO` |
| DMM.10 | I2C de sensores e capacidades de placa | `BLOQUEADO` |
| DMM.11 | Wi-Fi, USB, console e coexistência de periféricos | `BLOQUEADO` |
| DMM.12 | Boot completo, modos degradados e integração com o head | `BLOQUEADO` |
| DMM.13 | Soak integrado e cutover da main-controller | `BLOQUEADO` |

#### DMM.1 — inventário e matriz de recabeamento

- confirmar silk, módulo N32R16V e domínio de 1,8 V;
- fotografar/registrar a fiação atual antes de mover qualquer cabo;
- mapear origem, destino, tensão e direção de cada fio;
- identificar alimentação de INMP441, MAX98357A, TTLinker, LEDs e touch;
- registrar quais periféricos continuam desconectados;
- verificar conflitos entre pinout documentado, header físico e firmware;
- definir sequência de energização e rollback por periférico.

Saída obrigatória: matriz “recurso → pino Freenove legado → pino Waveshare →
tensão → alimentação → gate → rollback”.

Documento de execução e consolidação: `docs/DMM1_WAVESHARE_INVENTORY.md`.
Este corte fecha apenas o inventario documental; a implementação de DMM.2
entrou em validacao com o perfil Waveshare explicitamente selecionado no
build.

#### DMM.2 — perfil de placa e HAL

- tornar a seleção Freenove-legado/Waveshare explícita em Kconfig/build;
- impedir que o build Waveshare inclua pinos DVP, display ou SD locais;
- migrar HALs de corpo para `nb_main_hal` ou configuração de placa equivalente;
- corrigir `board_caps` para nome e capacidades reais da Waveshare;
- adicionar asserts de conflito entre link, áudio, servo, LED, touch e USB;
- manter o mapa Freenove apenas como fallback temporário até DMM.13.

Gate: os dois perfis compilam; o perfil Waveshare não toca GPIOs exclusivos do
head e o perfil legado continua reproduzível para rollback. Evidência atual:
build legado (`firmware/main-controller/build`) e build Waveshare
(`build_dm2_clean2` com `sdkconfig.dm2.profile2` +
`sdkconfig.dm2.defaults`) aprovados; os HALs de corpo passaram a consumir
`nb_hw_config_profile.h`, e `nb_hw_config_main.h` agora trava conflitos
explícitos entre link, áudio, servo, LED, touch e USB.

#### DMM.3 — alimentação e brownout

- definir fonte 5 V, corrente disponível, GND comum e distribuição;
- não unir saídas 5 V de fontes independentes;
- validar divisor externo antes de ligar GPIO7;
- bancada atual: divisor temporário 100k/100k validado em 2026-06-21;
- medir 3,3 V/5 V em idle, Wi-Fi TX, áudio e LEDs;
- testar brownout e desligamento seguro de periféricos;
- preservar boot da main sem head e sem periféricos externos.

Gate: nenhuma linha excede limites, nenhum back-power por GPIO e brownout não
causa movimento ou corrupção.

#### DMM.4 — áudio físico

- recabear INMP441 para SD 39, BCLK 40 e WS 41;
- recabear MAX98357A para DIN 42, BCLK 40 e WS 41;
- validar alimentação, L/R do microfone e ganho do amplificador;
- testar RX, TX e full-duplex I2S0 isoladamente;
- medir ruído, clipping, DC, underrun e estabilidade DMA;
- I2S DMA permanece em SRAM.

Gate: captura e reprodução PCM conhecidas, sem Wi-Fi, link ou display.

#### DMM.5 — Voice Audio v2

- validar `audio_io_service_v2`, playback, VAD e sessão de captura;
- wake word abre sessão somente pela política canônica;
- bridge recebe áudio real e devolve playback;
- medir latência, perda, cancelamento, follow-up e recuperação I2S;
- testar Wi-Fi + link do head + voz simultaneamente;
- nenhum fallback silencioso para caminhos legados da Freenove.

Gate: roteiro de voz completo, soak e testes definidos em
`docs/VOICE_AUDIO_V2_ARCHITECTURE.md`.

#### DMM.6 — touch corporal

- recabear fita/sensor para GPIO2 da Waveshare;
- recalibrar baseline, ruído, threshold e debounce na nova montagem;
- validar TAP, LONG, SUSTAINED, DEEP e CARESS;
- comprovar ausência de conflito com `HEAD_IRQ` da main em GPIO14;
- manter eventos e semântica existentes no event bus.

Gate: critérios da Etapa 2.2A repetidos na Waveshare após reboot e com Wi-Fi.

#### DMM.7 — LEDs

- recabear dois WS2812 externos para GPIO21 com level shift adequado;
- manter GPIO38 como LED onboard independente;
- definir qual LED serve ao produto e qual serve ao diagnóstico;
- testar brilho máximo, consumo, boot apagado e estados base;
- validar RMT concorrente com áudio, Wi-Fi e link.

Gate: animações existentes sem flicker, reset ou queda de alimentação.

#### DMM.8 — servo bus sem movimento

- recabear FE-TTLinker para UART1 TX17/RX18;
- confirmar direção TX/RX e níveis lógicos;
- manter alimentação dos servos desligada no primeiro gate;
- depois testar somente comunicação/PING com torque desabilitado;
- USB GPIO19/20 permanece livre e funcional;
- qualquer escrita de posição continua proibida.

Gate: leitura estável de IDs/telemetria com torque desabilitado e nenhum
movimento físico.

#### DMM.9 — motion safety

- concluir `docs/SERVO_SAFETY.md`;
- validar limites, heartbeat, stall, temperatura, tensão e brownout disable;
- comprovar que toda posição passa por `motion_safety_check_position()`;
- iniciar com amplitudes mínimas e área física livre;
- falha do head/link nunca altera a autoridade de veto.

Gate: somente o protocolo de liberação de safety autoriza movimento. Esta
subfase pode permanecer bloqueada sem impedir DMM.10–DMM.13 em modo sem servo.

#### DMM.10 — I2C e capacidades

- validar I2C0 GPIO4/5 e pull-ups externos;
- detectar apenas sensores realmente instalados;
- tratar IMU, bateria e sensores futuros como capabilities, não pressupostos;
- atualizar `board_caps` e diagnóstico;
- nenhum sensor ausente impede voz, link ou safety básico.

Gate: scan e drivers instalados não conflitam; itens adiados permanecem
desabilitados.

#### DMM.11 — coexistência

- Wi-Fi ativo com I2S, RMT, UART servo, touch e link;
- USB nativo e console preservados;
- medir stacks, SRAM interna, PSRAM e DMA;
- validar OTA/reboot sem deixar saída perigosa;
- confirmar que GPIO47/48 nunca são dirigidos como 3,3 V.

Gate: matriz de concorrência sem panic, watchdog, perda de áudio ou link.

#### DMM.12 — boot e modos degradados

- boot normal seleciona o perfil Waveshare;
- head ausente não impede áudio, touch, LEDs ou behavior;
- mic/speaker ausentes degradam voz sem derrubar o restante;
- touch/LED ausentes geram diagnóstico, não reboot;
- servo ausente mantém motion `DISABLED`;
- snapshot visual converge quando o head retorna.

Gate: roteiro de falhas por periférico e reboot isolado aprovado.

#### DMM.13 — soak e cutover

- 8 h com voz periódica, Wi-Fi, link, touch e LEDs;
- servos ficam desligados se DMM.9 não estiver aprovado;
- zero brownout, leak, watchdog, corrupção ou conflito de GPIO;
- perfil Waveshare vira default de produção;
- mapa Freenove monolítico fica somente como rollback versionado;
- os periféricos de corpo não voltam a ser conectados ao head.

**Gate da trilha DMM:** DMM.1–DMM.8 e DMM.10–DMM.13 `FEITO`. DMM.9 é
obrigatória apenas para habilitar movimento real, não para concluir a migração
da main com servos permanentemente desarmados.

### DM2 — Autoridade visual completa no head

**Objetivo:** fazer o head ser a única superfície de render do produto, com a
main mantendo apenas decisão e estado semântico.

DM2 não termina com “uma face remota aparecendo”. Termina quando todos os
estados, faces, animações, overlays e textos relevantes têm paridade, recovery
e um único dono de render.

| ID | Entrega | Status |
| --- | --- | --- |
| DM2.1 | Rota semântica e capability sem display físico | `FEITO` |
| DM2.2 | HAL ST7789, framebuffer PSRAM e gate elétrico | `FEITO` |
| DM2.3 | Renderer procedural inicial e gaze básica | `FEITO` |
| DM2.4 | Facade visual e oito overlays compactos | `FEITO` |
| DM2.5 | Reboot isolado, head ausente e restauração de snapshot | `FEITO` |
| DM2.6 | Inventário visual congelado e matriz de paridade | `PRONTO` |
| DM2.7 | Contrato visual v2 modular e testes host | `BLOQUEADO` |
| DM2.8 | Paridade geométrica das faces e expressões | `BLOQUEADO` |
| DM2.9 | Motor de animação, blink, gaze, tilt e timelines | `BLOQUEADO` |
| DM2.10 | Estados visuais e transições da FSM | `BLOQUEADO` |
| DM2.11 | Overlays, texto, timers, toast e status rail | `BLOQUEADO` |
| DM2.12 | Assets, fontes, ícones e política de memória | `BLOQUEADO` |
| DM2.13 | Recovery visual, expiração de transientes e fallback | `BLOQUEADO` |
| DM2.14 | Paridade de produto, latência e soak visual | `BLOQUEADO` |
| DM2.15 | Cutover: head como render padrão; fallback de release | `BLOQUEADO` |

#### DM2.6 — inventário visual congelado

Mapear, sem alterar comportamento:

- todas as expressões `nb_expression_t` e seus parâmetros;
- todos os 11 estados `NB_STATE_*`;
- blink simples, duplo, assimétrico e preservação de expressão;
- gaze, glance, anchor, drift, saccade, tilt e motifs de idle;
- animações de wake, sleeping, listening, speaking, touch e erro;
- overlays do `ui_overlay_service`, texto, toast, timer e status rápido;
- status persistentes e respectivas prioridades;
- bbox/preview de visão, marcados como dependência futura DM4;
- produtores de cada estado visual e seu owner.

Saída obrigatória: matriz “feature → produtor main → representação no contrato
→ renderer head → teste → status”. Depois do congelamento, qualquer feature
visual nova entra primeiro nessa matriz, sem criar subfase.

#### DM2.7 — contrato visual v2

- substituir a struct visual monolítica limitada por mensagens modulares;
- manter compatibilidade temporária com protocolo visual v1;
- fila main→link; nenhuma chamada externa direta ao `nb_link_engine`;
- snapshots completos idempotentes por `generation`;
- comandos transitórios com ID, deadline e política de cancelamento;
- resposta/telemetria de aplicação para erros relevantes;
- negociação de capability por bloco visual.

Gate: testes host cobrem versões, campos reservados, geração, transientes,
reboot, backpressure e compatibilidade v1/v2.

#### DM2.8 — faces e expressões

- portar para o head o modelo paramétrico canônico, não apenas dez desenhos
  aproximados;
- preservar assimetria, abertura, squint, curvas, cor e interpolação;
- comparar todas as expressões em capturas lado a lado;
- definir tolerância visual e golden scenes;
- nenhum tipo LovyanGFX cruza o contrato.

Gate: catálogo completo reproduzido no head e aprovado visualmente.

#### DM2.9 — animação e gaze

- blink simples, duplo e assimétrico;
- animação preserva expressão base e overlays;
- gaze target/anchor/glance com timing reproduzível;
- micro-drift, saccades e tilt sustentado;
- motifs compostos de `IDLE_REFERENCE.md`;
- prioridade e interrupção de timelines;
- renderer continua responsivo durante tráfego do link.

Gate: sessões IDLE/ATTENTIVE de 60 s atendem os critérios de
`docs/IDLE_REFERENCE.md`, sem drift de estado após reconexão.

#### DM2.10 — estados visuais

Implementar e validar uma ficha visual para cada estado:

| Estado | Base esperada |
| --- | --- |
| `BOOT_UP` | Sequência segura até snapshot válido |
| `IDLE` | Neutral, gaze central, transientes limpos |
| `ATTENTIVE` | Atenção sem substituir permanentemente IDLE |
| `RESPONDING` | Fala/atividade com retorno limpo |
| `TOUCH_REACTING` | Reação transitória correlacionada |
| `SLEEPING` | Base própria, wake determinístico |
| `ERROR` | Alerta legível sem ocultar informação crítica |
| `SAFE_MODE` | Estado seguro inequívoco |
| `MEDITATION` | Base calma e mic bloqueado |
| `SILENT_COMPANY` | Companhia silenciosa sem parecer desligado |
| `MAINTENANCE` | Diagnóstico explícito e timeout |

Gate: matriz completa de entrada, permanência, saída, reboot e retorno a IDLE.

#### DM2.11 — overlays, texto e status

- migrar os oito overlays existentes sem regressão;
- migrar status rail e status rápido;
- suportar ícones persistentes, toast e texto limitado;
- definir slots, z-order, overflow, severidade e expiração;
- nenhum serviço desenha diretamente;
- texto e ícones não cobrem olhos, boca ou preview crítico.

Gate: todos os critérios da Etapa 16.2 executados no renderer do head.

#### DM2.12 — assets e memória

- fonte editável separada do formato runtime;
- IDs estáveis para ícones/expressões/assets;
- assets essenciais em flash do head;
- cache e assets opcionais no SD entram somente após DM5;
- framebuffer e sprites em PSRAM;
- medir PSRAM, SRAM interna, DMA, stack e fragmentação.

Gate: mínimo de 300 KB de PSRAM livre além dos buffers ativos e zero alocação
por frame no render loop.

#### DM2.13 — recovery e fallback

- snapshot persistente restaurado após reboot do head;
- transientes vencidos não reaparecem;
- novo boot ID invalida timelines e objetos temporários;
- head sem main mostra neutro/offline após timeout;
- main sem head continua voz, comportamento e safety;
- falha do HAL visual não anuncia capability falsa;
- durante desenvolvimento, fallback local é explícito por configuração.

Gate: reboot/desconexão em cada estado da DM2.10 e retorno visual correto.

#### DM2.14 — paridade e soak

- golden scenes de expressões, estados e overlays;
- roteiro visual filmável e repetível;
- p95 intenção→frame menor que 20 ms sem BULK;
- 8 h com animação, transições, reconnect e telemetria;
- zero corrupção, piscar, leak, panic, watchdog ou perda de baseline;
- operador confirma visualmente os cenários, não apenas logs.

#### DM2.15 — cutover visual

- head vira rota padrão de release;
- main conserva fallback somente por uma janela de rollback definida;
- nenhum feature novo entra no renderer local;
- documentação e testes apontam para o head;
- remoção física do render legado ainda pertence a DM6.

**Gate da fase DM2:** DM2.1–DM2.15 `FEITO`. Só então DM4 pode voltar à ordem
ativa.

### DM3 — Touchscreen do head

**Objetivo:** adquirir touch localmente e manter decisão comportamental na
main. Esta fase depende da instalação de um painel/controlador touchscreen e
não bloqueia DM4 se o hardware continuar ausente.

| ID | Entrega | Status |
| --- | --- | --- |
| DM3.1 | Hardware/endereço/pinout e capability | `BLOQUEADO` |
| DM3.2 | HAL, calibração, debounce e eventos crus | `BLOQUEADO` |
| DM3.3 | Contrato EVENT e sincronização temporal | `BLOQUEADO` |
| DM3.4 | Roteamento no main e conflitos com touch corporal | `BLOQUEADO` |
| DM3.5 | Latência, ruído, reconnect e soak | `BLOQUEADO` |

Gate: evento visual p95 menor que 30 ms, sem ação autônoma no head.

### DM4 — Câmera no head e integração de visão

**Objetivo:** fazer o head ser dono do OV2640, preview e entrega de frames,
mantendo análise semântica no server.

O trabalho existente é preservado como spike físico. Está `PAUSADO` até DM2.15.
Antes de retomar, os defeitos registrados na revisão de 2026-06-20 devem ser
corrigidos dentro dos IDs existentes: ownership single-thread do link,
correlação por request ID, capability após falha e backpressure explícito.

| ID | Entrega | Status |
| --- | --- | --- |
| DM4.1 | Contrato semântico inicial | `FEITO` |
| DM4.2 | Receptor semântico no head | `FEITO` |
| DM4.3 | Cliente main enfileirado e correlação robusta | `PAUSADO` |
| DM4.4 | Probe ponta a ponta confiável | `PAUSADO` |
| DM4.5 | HAL DVP/I2C e captura física; evidência parcial preservada | `PAUSADO` |
| DM4.6 | Isolamento da task, falhas e backpressure | `PAUSADO` |
| DM4.7 | Preview local composto pelo renderer do head | `BLOQUEADO` |
| DM4.8 | BULK JPEG sob demanda e encaminhamento ao server | `BLOQUEADO` |
| DM4.9 | Bbox, presença e gaze integrados sem decisão no head | `BLOQUEADO` |
| DM4.10 | Recovery, câmera ausente, display concorrente e soak | `BLOQUEADO` |
| DM4.11 | Cutover do pipeline de visão | `BLOQUEADO` |

Regras específicas:

- preview nunca atravessa o link frame a frame;
- main não mantém DVP nem framebuffer da câmera;
- JPEG só via BULK e por pedido explícito;
- ausência de câmera desabilita visão, não significa “usuário ausente”;
- resposta de cada request é correlacionada e consumida uma única vez;
- fila cheia retorna `BUSY`; não sobrescreve requests silenciosamente;
- capability representa serviço operacional, não apenas flag configurada.

Gate da fase: preview local, JPEG real no server, presença/reconhecimento
preservados, câmera/display concorrentes, reconnect e soak aprovados.

### DM5 — Storage remoto no head

**Objetivo:** tornar o head o único dono do microSD sem introduzir I/O
bloqueante ou perda silenciosa.

| ID | Entrega | Status |
| --- | --- | --- |
| DM5.1 | Contrato STORAGE, handles e sandbox de paths | `BLOQUEADO` |
| DM5.2 | Worker SD do head e telemetria do volume | `BLOQUEADO` |
| DM5.3 | Cliente assíncrono e backpressure no main | `BLOQUEADO` |
| DM5.4 | Leitura BULK, assets e cache | `BLOQUEADO` |
| DM5.5 | Escrita, sync e commit atômico | `BLOQUEADO` |
| DM5.6 | Logs e diagnósticos remotos | `BLOQUEADO` |
| DM5.7 | LTM, fila offline e replay idempotente | `BLOQUEADO` |
| DM5.8 | Áudio remoto com prebuffer/refill | `BLOQUEADO` |
| DM5.9 | SD ausente/cheio/removido e power-loss | `BLOQUEADO` |
| DM5.10 | Soak concorrente e cutover | `BLOQUEADO` |

Regras específicas:

- nenhum FATFS/SDMMC no main após cutover;
- enqueue não equivale a persistência;
- somente commit/sync confirmado encerra operação durável;
- nenhuma escrita síncrona em task de prioridade ≥ 10;
- áudio crítico pequeno conserva fallback em flash;
- CONTROL/EVENT preemptam BULK;
- handles incluem geração e boot ID.

Gate da fase: power-loss, saturação, replay, áudio e ausência de SD aprovados.

### DM6 — Remoção do legado e consolidação

**Objetivo:** remover duplicação somente depois de todos os substitutos estarem
verdes.

| ID | Entrega | Status |
| --- | --- | --- |
| DM6.1 | Auditoria de dependências e plano de remoção | `BLOQUEADO` |
| DM6.2 | Remover display/LovyanGFX/render do main | `BLOQUEADO` |
| DM6.3 | Remover câmera/DVP/preview do main | `BLOQUEADO` |
| DM6.4 | Remover SD/FATFS local do main | `BLOQUEADO` |
| DM6.5 | Remover flags, probes e compatibilidade vencida | `BLOQUEADO` |
| DM6.6 | Atualizar arquitetura, builds, testes e matriz final | `BLOQUEADO` |
| DM6.7 | Soak final, rollback de release e encerramento | `BLOQUEADO` |

DM6 não recebe funcionalidade nova. Qualquer feature descoberta retorna à fase
dona do domínio.

## 9. Ordem obrigatória

```text
DM0 FEITO
  └─ DM1 FEITO
       ├─ DMM.1 → DMM.13  (corpo e autoridade principal na Waveshare)
       └─ DM2.6 → DM2.15  (autoridade visual no head)
            └─ DM3, somente quando houver touchscreen

DMM.13 + DM2.15
       └─ DM4.3 → DM4.11
            └─ DM5.1 → DM5.10
                 └─ DM6.1 → DM6.7
```

DM4.1/DM4.2 e o spike DM4.5 já existem, mas não autorizam avançar DM4 enquanto
DM2 e DMM não estiverem concluídas. DM5 não inicia antes do gate final de DM4.

DMM e DM2 são trilhas tecnicamente independentes após DM1. Para evitar expansão
de escopo, apenas um ID fica `EM ANDAMENTO` por vez, salvo autorização explícita
do usuário para trabalho paralelo.

## 10. Gates comuns a todas as subfases

Cada subfase aplicável exige:

- build main/head com `-Wall -Wextra -Werror`;
- testes host dos contratos tocados;
- `git diff --check`;
- caminho normal e caminho degradado;
- reboot isolado e novo boot ID;
- fila cheia/backpressure;
- timeout e resultado ambíguo;
- telemetria suficiente para localizar perda;
- nenhuma regressão de `motion_safety`, áudio ou baseline IDLE;
- documentação de rollback;
- evidência com data e comandos usados.

Subfases de hardware exigem ainda:

- perfis de bancada separados dos defaults;
- confirmação das duas portas/MCUs;
- logs dos dois lados quando o fluxo é ponta a ponta;
- inspeção visual ou medição física quando o critério não é observável por log;
- retorno dos MCUs ao perfil seguro ao fim da sessão.

## 11. Estratégia de commits e rollback

- Um commit por subfase ou correção coesa dentro dela.
- Código, testes e documentação do mesmo gate podem ficar juntos quando o
  rollback precisa ser atômico.
- Perfis `sdkconfig.dm*.defaults` nunca viram defaults de produção antes do
  cutover.
- Não apagar evidência de bring-up; corrigir conclusões incorretas com errata.
- Toda remoção de legado deve apontar para o commit anterior que ainda oferece
  fallback funcional.

## 12. Próxima ação autorizada

Os próximos IDs prontos são:

- **DMM.1 — inventário elétrico e matriz de recabeamento da Waveshare**;
- **DM2.6 — inventário visual congelado e matriz de paridade**.

Por prioridade de fundação, iniciar DMM.1 antes de alterar o build normal. DM2.6
pode ser executada depois ou em paralelo somente com autorização explícita.

Até DM2.15:

- DM4 permanece pausada, exceto correção de segurança que impeça build normal;
- DM5 não inicia;
- DM6 não remove legado;
- o perfil normal não muda para Waveshare antes de DMM.13;
- a Etapa 16.2 deve ser implementada já pensando no renderer do head, não como
  uma nova dependência permanente do render local.
