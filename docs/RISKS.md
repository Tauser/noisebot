# NoiseBot — Riscos Criticos do Projeto

## Sumario de Risco

| ID  | Categoria     | Descricao resumida                                | Severidade |
|-----|---------------|---------------------------------------------------|------------|
| R01 | Eletrico      | Brownout sob carga de servos                      | Alta       |
| R02 | Eletrico      | Ripple do boost afetando sensores                 | Media      |
| R03 | Eletrico      | Overdischarge da LiPo                             | Alta       |
| R04 | Mecanico      | Stall de servo sem deteccao                       | Alta       |
| R05 | Mecanico      | Posicao invalida por bug de software              | Media      |
| R06 | Mecanico      | Pico de corrente em aceleracao                    | Media-Alta |
| R07 | Arquitetura   | Acoplamento prematuro entre servicos              | Alta       |
| R08 | Arquitetura   | NVS sem versionamento de schema                   | Media      |
| R09 | Temporizacao  | Conflito de DMA camera vs I2S                     | Media-Alta |
| R10 | Temporizacao  | WS2812 glitch por interrupcao durante RMT         | Baixa      |
| R11 | Memoria       | Fragmentacao de PSRAM                             | Alta       |
| R12 | Memoria       | Stack overflow silencioso em FreeRTOS             | Alta       |
| R13 | Memoria       | Pressao de SRAM interna — DMA buffers multiplos   | Alta       |
| R14 | Integracao    | Regressao ao integrar novo subsistema             | Media      |
| R15 | Debug         | Ausencia de logging persistente nas fases iniciais| Alta       |
| R16 | Eletrico      | Nivel logico 3.3V marginal para WS2812            | Baixa      |

---

## Detalhamento dos Riscos

---

### R01 — Brownout sob carga de servos

**Categoria:** Eletrico
**Severidade:** Alta

**Descricao:**
Dois servos SCS0009 em aceleracao ou stall simultaneo podem puxar 2-3A do boost TPS61088. Dependendo da resistencia interna da LiPo e do estado de carga, a tensao de bateria pode cair abaixo do threshold de brownout do ESP32-S3. Isso causa reset inesperado com potencial de corrupcao de NVS e perda de estado dos servos.

**Sinais de manifestacao:**
- Reset inesperado com reset reason = ESP_RST_BROWNOUT durante movimento
- Sistema reinicia no meio de uma sequencia de movimento
- NVS corrompida apos reset

**Mitigacao:**
- Etapa 2.1 obrigatoria: medir tensao do sistema durante movimento de 2 servos antes de integrar ao comportamento
- Configurar brownout threshold adequado (nivel 7, ~2.97V) com base em medicoes reais
- Capacitor de bulk na entrada dos servos (470µF-1000µF) para absorver transientes
- Safety layer bloqueia movimento se SoC < CRITICAL_PCT
- Nao comandar dois servos para posicoes extremas simultaneamente nos primeiros testes

**Status:** Mitigar na Etapa 2.1

---

### R02 — Ripple do boost afetando sensores

**Categoria:** Eletrico
**Severidade:** Media

**Descricao:**
O TPS61088 boost converter sob carga variavel (servos acelerando e desacelerando) gera ripple na saida de 5V. Se o plano de terra ou o layout de PCB nao isolar adequadamente, esse ripple pode aparecer no barramento de 3.3V e afetar leituras de I2C (MAX17048, bq25185, MPU-6050) e potencialmente o ADC interno.

**Sinais de manifestacao:**
- Leituras erraticas do fuel gauge correlacionadas com movimento de servo
- Dados do MPU-6050 com spike durante aceleracao de servo
- Erros NAK em I2C durante movimento

**Mitigacao:**
- Medir ripple com osciloscópio durante Etapa 2.1 (criterio: < 50mV no 5V)
- Verificar que ripple do 5V nao aparece no 3.3V (plano de terra separado)
- Adicionar capacitores de bypass (100nF ceramico + 10µF eletrolit) em cada dispositivo I2C se ripple for problema
- Histerese no fuel gauge: ignorar leituras de SoC que diferem > 5% da leitura anterior em < 1s

**Status:** Verificar na Etapa 2.1

---

### R03 — Overdischarge da LiPo

**Categoria:** Eletrico
**Severidade:** Alta (dano permanente a celula)

**Descricao:**
Celula LiPo abaixo de 2.5V sofre dano quimico irreversivel que reduz capacidade e aumenta risco de incendio em cargas futuras. O bq25185 tem protecao de undervoltage, mas nao deve ser a unica linha de defesa — especialmente porque o sistema pode ser usado sem carregador conectado.

**Mitigacao:**
- PowerManager com shutdown policy conservadora: iniciar desligamento de perifericos pesados com SoC < 10% e tensao < 3.1V
- Threshold de shutdown configurado ANTES de atingir limite de dano da celula (3.1V vs 2.5V de dano)
- LEDs e display alertam usuario com antecedencia (LOW = 20%, CRITICAL = 10%)
- Brownout threshold configurado como ultima linha de defesa
- Nunca deixar bateria completamente descarregada por longos periodos (armazenar a ~50%)

**Status:** Implementar na Etapa 0.3

---

### R04 — Stall de servo sem deteccao

**Categoria:** Mecanico
**Severidade:** Alta

**Descricao:**
Servo empurrado contra limite mecanico fisico (peca travada, cabo preso, obstrucao) consome corrente de stall (~1.5A) indefinidamente. Sem deteccao ativa por software, o servo pode superaquecer e queimar em segundos a minutos.

**Mitigacao:**
- Monitoramento continuo de load (registro `Present Load` a cada 10ms)
- Stall detectado apos N leituras consecutivas com load > OVERLOAD_THRESHOLD_PCT (default: 80%, N=5)
- Acao imediata: torque-off + publicar EVT_SERVO_STALL + estado → FAULT
- Monitoramento de temperatura simultaneo como segunda camada
- Teste de stall simulado obrigatorio na Etapa 3.2 (segurar servo manualmente)

**Status:** Implementar na Etapa 3.2

---

### R05 — Posicao invalida por bug de software

**Categoria:** Mecanico
**Severidade:** Media

**Descricao:**
Bug no BehaviorFSM ou em codigo de comportamento pode gerar valor de posicao fora da faixa mecanica real do robo (diferente dos limites eletrônicos do servo). O mecanismo plastico pode quebrar se forcado alem de seu range real.

**Mitigacao:**
- Hard limits de posicao no ServoSafetyLayer independentes do codigo de comportamento
- Limites mecanicos fisicos documentados e mapeados para steps antes da Etapa 3.2
- SafetyLayer clipa posicao ao limite (nao rejeita — clipa e loga), garantindo sempre comando valido
- Teste de limite obrigatorio na Etapa 3.2

**Status:** Implementar na Etapa 3.2

---

### R06 — Pico de corrente em aceleracao

**Categoria:** Mecanico / Eletrico
**Severidade:** Media-Alta

**Descricao:**
Comando de posicao com step grande (ex: 0° para 300° instantaneamente) causa pico de corrente significativo durante aceleracao. Dois servos com step grande simultâneo pode causar brownout.

**Mitigacao:**
- Rampa de aceleracao obrigatoria: MAX_STEP_PER_TICK limita delta de posicao por ciclo
- Nao comandar dois servos para posicoes extremas simultaneamente
- Primeira sequencia de testes: movimentos pequenos e graduais

**Status:** Implementar na Etapa 3.2

---

### R07 — Acoplamento prematuro entre servicos

**Categoria:** Arquitetura
**Severidade:** Alta (impacto em todo o desenvolvimento futuro)

**Descricao:**
Servicos que chamam APIs internas de outros diretamente criam dependencias ciclicas, dificultam teste unitario, tornam impossivel substituir um servico e geram bugs silenciosos de estado. Uma vez que o acoplamento existe e varios servicos dependem dele, desfaze-lo custa semanas.

**Sinais de manifestacao:**
- Arquivo de servico importa header de outro servico (fora do event bus e API publica)
- Funcao em servico A chama funcao interna de servico B
- Servico nao pode ser compilado/testado sem instanciar servico diferente

**Mitigacao:**
- Event bus desde a Etapa 0.2 como mecanismo principal de comunicacao
- Code review checklist: nenhum servico pode incluir header interno de outro
- Revisao arquitetural antes de cada novo servico

**Status:** Prevenir desde Etapa 0.2

---

### R08 — NVS sem versionamento de schema

**Categoria:** Arquitetura
**Severidade:** Media

**Descricao:**
Mudanca de nome de chave, tipo ou namespace em NVS sem logica de migracao resulta em config corrompida ou inconsistente apos atualizacao de firmware. Em campo, isso pode forcar erase de NVS e perda de calibracoes.

**Mitigacao:**
- ConfigManager com versao de schema (chave `nb_system/schema_ver`)
- Funcao de migracao chamada no boot se versao atual != versao do firmware
- Convencao rigorosa de nomenclatura: nunca renomear chave sem escrever migracao

**Status:** Implementar na Etapa 0.2

---

### R09 — Conflito de DMA camera vs I2S

**Categoria:** Temporizacao
**Severidade:** Media-Alta

**Descricao:**
OV2640 via DVP usa DMA de alta largura de banda para transferencia de frames. INMP441 e MAX98357A usam DMA I2S. Em operacao simultanea, o arbitrador de DMA pode causar starvation de um dos canais, resultando em drop de amostras de audio ou frames corrompidos.

**Sinais de manifestacao:**
- Drop de amostras de audio correlacionado com captura de frame
- Frame de camera com linhas corrompidas durante playback de audio
- I2S DMA timeout errors nos logs

**Mitigacao:**
- Camera entra somente na Etapa 6.1, apos audio estar estavel
- Teste explicito de camera + audio simultâneos antes de integrar comportamento
- Se conflito ocorrer: aumentar tamanho dos buffers DMA ou adicionar delay entre captura e reproducao

**Status:** Testar na Etapa 6.1

---

### R10 — WS2812 glitch por interrupcao

**Categoria:** Temporizacao
**Severidade:** Baixa

**Descricao:**
WS2812 e sensivel a timing de 1.25µs. Interrupcao durante transmissao RMT pode corromper sequencia de cor. O RMT mitiga a maior parte, mas interrupcoes de alta prioridade (NMI, wdt) durante envio podem causar glitch.

**Mitigacao:**
- Usar canal RMT dedicado para WS2812, nao compartilhar
- Nao executar operacoes de alta latencia (erase NVS, format SD) durante animacoes de LED

**Status:** Verificar durante Etapa 1.2

---

### R11 — Fragmentacao de PSRAM

**Categoria:** Memoria
**Severidade:** Alta

**Descricao:**
PSRAM alocada e liberada frequentemente (frames de camera, buffers de audio) pode fragmentar o heap de PSRAM. Fragmentacao pode fazer com que alocacoes grandes subsequentes falhem mesmo com espaco total suficiente.

**Mitigacao:**
- Usar pools de tamanho fixo para alocacoes frequentes (frame buffers de camera em pool pre-alocado)
- HeapMonitorTask monitora heap livre e fragmentacao a cada 60s
- Alerta se PSRAM livre < 200KB
- Liberar frame buffer imediatamente apos uso (nao acumular frames)

**Status:** Prevenir desde Etapa 1.1 (framebuffer) e monitorar em todas as etapas

---

### R12 — Stack overflow silencioso

**Categoria:** Memoria
**Severidade:** Alta

**Descricao:**
FreeRTOS com stack muito pequeno pode corromper memoria adjacente silenciosamente sem crash imediato. Os sintomas aparecem muito depois como comportamento erratico ou corrupcao de dados de outra task.

**Mitigacao:**
- `configCHECK_FOR_STACK_OVERFLOW=2` habilitado em desenvolvimento (modo pesado mas seguro)
- Superdimensionar stacks inicialmente (2x o estimado) e afinar com `uxTaskGetStackHighWaterMark()` depois
- Verificar high watermark de todas as tasks na Etapa 8.1

**Status:** Configurar na Etapa 0.1

---

### R13 — Pressao de SRAM interna

**Categoria:** Memoria
**Severidade:** Alta

**Descricao:**
Buffers de DMA devem estar em SRAM interna (DMA nao pode acessar PSRAM). Camera DVP + I2S0 + I2S1 + SPI (display) tem buffers de DMA simultaneos. Com 512KB fragmentados em bancos, a pressao e real.

**Estimativa de DMA buffers em SRAM:**
- I2S0 (mic): 2x buffer de 512 bytes = 1KB
- I2S1 (speaker): 2x buffer de 512 bytes = 1KB
- SPI display: buffer de linha = ~480 bytes
- Camera DVP: buffer de linha(s) em SRAM interna (tamanho varia)
- FreeRTOS stacks: ~30-40KB total estimado

**Mitigacao:**
- Mapear consumidores de SRAM interna antes da Etapa 6.1
- Camera entra por ultimo justamente para verificar que SRAM nao esta esgotada
- HeapMonitorTask alerta se SRAM < 20KB livre

**Status:** Monitorar desde Etapa 0.1

---

### R14 — Regressao ao integrar novo subsistema

**Categoria:** Integracao
**Severidade:** Media

**Descricao:**
Adicionar camera pode causar starvation de task de servo por pressao de CPU ou DMA. Adicionar audio pode interferir com timing de touch. A interacao entre subsistemas pode quebrar criterios de aceitacao ja aprovados de etapas anteriores.

**Mitigacao:**
- Apos cada novo subsistema integrado, re-executar testes basicos dos subsistemas anteriores
- HeapMonitorTask e TaskMonitor logar continuamente para detectar degradacao
- Servos: re-testar heartbeat e safety layer apos integracao de audio e camera

**Status:** Prevenir com re-testes apos cada integracao

---

### R15 — Ausencia de logging persistente nas fases iniciais

**Categoria:** Debug
**Severidade:** Alta

**Descricao:**
Sem microSD operacional nas primeiras fases, brownouts e crashes apagam todos os logs de UART. Bugs de boot com reset loop sao impossiveis de depurar sem logs persistentes.

**Mitigacao:**
- Boot flags e crash_count em RTC memory desde a Etapa 0.1 (persiste apos brownout)
- microSD e log rotation implementados na Etapa 1.3 antes de qualquer periferico de risco
- Logger com buffer circular em RAM que sobrevive brevemente — suficiente para capturar stack trace antes de crash

**Status:** Mitigar desde Etapa 0.1 (RTC memory) e completar na Etapa 1.3

---

### R16 — Nivel logico 3.3V marginal para WS2812

**Categoria:** Eletrico
**Severidade:** Baixa

**Descricao:**
WS2812B especifica Vih > 0.7 * Vcc. Com Vcc = 5V, Vih > 3.5V. O ESP32-S3 opera com GPIO a 3.3V. Na pratica, a maioria dos WS2812B funciona com 3.3V de HIGH, mas alguns lotes sao mais sensiveis.

**Mitigacao:**
- Testar na Etapa 1.2: se glitches ou cores erradas → adicionar SN74HCT1G125 ou level shifter simples de 3.3V → 5V
- Solucao alternativa: alimentar WS2812 com 3.3V (Vcc = 3.3V → Vih > 2.31V → 3.3V OK), mas brilho reduzido

**Status:** Verificar durante Etapa 1.2
