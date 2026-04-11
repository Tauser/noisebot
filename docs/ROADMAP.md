# NodeBot — Roadmap e Etapas de Desenvolvimento

## Roadmap Macro

```
BLOCO 0 — Fundacao
  0.1  Boot, Logging, Watchdog, Boot Safety
  0.2  ConfigManager e Event Bus
  0.3  Power Monitor (MAX17048 + bq25185)
  → Marco: base sólida observavel e segura

BLOCO 1 — Bring-up de hardware low-risk
  1.1  Display ST7789
  1.2  LEDs WS2812
  1.3  Storage microSD

BLOCO 2 — Validacao de power path
  2.1  Medicoes fisicas do power path completo
  → Marco: power path caracterizado e validado

BLOCO 3 — Servos (controlado)
  3.1  Servo driver (comunicacao e status — SEM movimento)
  3.2  Servo motion safety layer
  → Marco: servos seguros para uso progressivo

BLOCO 4 — Sensores
  4.1  Touch sensor
  4.2  IMU (MPU-6050)

BLOCO 5 — Audio
  5.1  Captura de audio (INMP441)
  5.2  Playback de audio (MAX98357A)

BLOCO 6 — Camera
  6.1  OV2640 bring-up com gestao de memoria

BLOCO 7 — Comportamento e Persona
  7.1  FSM de comportamento e orquestracao de servicos

BLOCO 8 — Integracao total e validacao longa
  8.1  Testes de carga, stress, autonomia, regressao
```

---

## Etapas Detalhadas

---

### ETAPA 0.1 — Boot, Logging, Watchdog, Boot Safety

**Objetivo:** Sistema arranca de forma observavel e controlada desde o primeiro segundo.

**Por que primeiro:** Impossivel depurar qualquer coisa sem logging. Impossivel confiar em qualquer sistema sem watchdog. Todo o restante do projeto depende dessa fundacao.

**Escopo que entra:**
- UART de debug: 921600 baud, configurada antes de qualquer outra coisa
- Logger estruturado: niveis (DEBUG/INFO/WARN/ERROR/CRITICAL), timestamp, modulo de origem
- Hardware WDT via `esp_task_wdt_init()` — registrar task principal imediatamente
- RTC memory struct com: boot_count, crash_count, last_reset_reason, brownout_flag, safe_mode_flag
- Deteccao de reset reason via `esp_reset_reason()` — logar e persistir em RTC memory
- Boot Safety: se crash_count >= 3, ativar SAFE_MODE
- Brownout callback: registrar handler que loga e marca flag antes do reset
- Boot Report: struct publicada no event bus ao final do boot

**Fora do escopo:**
- Qualquer periferico externo
- Display, LEDs, microSD
- NVS (entra em 0.2)

**Entregaveis:**
- `infra/logger.h/c` com buffer circular e API de logging
- `infra/boot_manager.h/c` com fases de boot e boot safety
- `infra/watchdog_service.h/c` com registro de tasks e heartbeat

**Dependencias tecnicas:**
- Nenhuma — e a primeira etapa

**Riscos:**
- NVS corrompida em board virgem → tratar com erase + reinit em 0.2
- Logger com mutex pesado pode deadlock se chamado de ISR → proibir explicitamente (macro assert)
- Stack muito pequeno para task principal → superdimensionar inicialmente

**Criterios de aceitacao:**
- Boot loga motivo de reset correto em todos os casos: power-on, WDT, brownout, panic
- Boot counter persiste entre resets (RTC memory)
- crash_count incrementa corretamente em crash simulado
- Safe mode ativa apos 3 crashes consecutivos simulados
- Watchdog dispara e gera reset se task principal travar por 10s

**Testes minimos:**
1. Power-on: verificar log "RESET_REASON: POWER_ON"
2. Forcar WDT: travar loop principal — verificar reset e log correto
3. Simular crash: repetir 3x sem boot limpo — verificar safe mode

**Depende de hardware real:** Sim (target ESP32-S3)

**Sinais de imaturidade:**
- Qualquer reboot silencioso nao logado
- Watchdog configurado mas nunca testado deliberadamente

---

### ETAPA 0.2 — ConfigManager e Event Bus

**Objetivo:** Infraestrutura de comunicacao interna e configuracao centralizada operacionais.

**Por que aqui:** Event bus desacopla todos os servicos que virao depois. Config manager e usado por todos os modulos. Implementar antes de qualquer modulo depender de solucao ad-hoc.

**Escopo que entra:**
- NVS inicializado com schema versionado
- `infra/config_manager.h/c`: API sobre NVS com namespace por modulo, defaults, migracao de versao
- `infra/event_bus.h/c`: fila tipada via FreeRTOS Queue
- Tipos de eventos base: SYSTEM_BOOT_COMPLETE, POWER_LOW, POWER_CRITICAL, ERROR_REPORTED, SERVICE_STARTED, SERVICE_FAILED
- EventDispatchTask: task dedicada para dispatch de eventos (Core 1)

**Fora do escopo:**
- Eventos de hardware especifico (entram com cada driver)
- Persistencia de eventos

**Entregaveis:**
- `infra/config_manager.h/c` com testes unitarios de read/write/default/migrate
- `infra/event_bus.h/c` com API de publish/subscribe
- Header de tipos de evento `infra/nb_events.h`

**Riscos:**
- Queue bloqueante pode causar prioridade inversao → publicacao com timeout de 10ms, nunca indefinidamente
- Namespace collision em NVS → convencao rigorosa de nomenclatura: "nb_" + modulo

**Criterios de aceitacao:**
- Config persiste entre resets e carrega defaults quando chave ausente
- Config schema versao lida corretamente
- Evento publicado entregue ao subscriber em < 1ms em condicao idle
- Nenhuma chamada bloqueante em ISR context

---

### ETAPA 0.3 — Power Monitor

**Objetivo:** Sistema conhece o estado de energia em tempo real antes de qualquer periferico pesado ser ligado.

**Por que antes de tudo mais:** Brownout sem politica de reacao = dano potencial a servos + perda de dados. O sistema precisa saber o estado de energia antes de qualquer decisao de inicializacao.

**Escopo que entra:**
- Driver I2C para MAX17048: leitura de SoC (%), tensao (mV), taxa de descarga (mV/h)
- Driver I2C para bq25185: status de carregamento, fonte ativa, faults (OVP, OCP, NTC)
- `infra/power_manager.h/c`: estado global (CHARGING, DISCHARGING, LOW, CRITICAL, FAULT)
- Thresholds configurados via ConfigManager: LOW_PCT=20, CRITICAL_PCT=10, SHUTDOWN_MV=3100
- Acoes em LOW: publicar EVT_POWER_LOW, ligar LED de aviso
- Acoes em CRITICAL: publicar EVT_POWER_CRITICAL, desligar perifericos pesados, preparar shutdown
- PowerMonitorTask: periodicidade de 1s, prioridade alta
- Brownout callback registrado com handler que loga e escreve em RTC memory antes do reset

**Dependencias tecnicas:**
- Logger (0.1), ConfigManager e EventBus (0.2)
- I2C bus inicializado com 400kHz e pull-ups corretos

**Riscos:**
- MAX17048 requer aprendizado de ciclo para precisao maxima → aceitar imprecisao inicial, documentar
- bq25185 pode reportar faults espurios no primeiro boot apos solda → tolerancia configuravel (ignora primeiros 3 faults)
- I2C pode ter problemas de pull-up → verificar valores de resistor na placa

**Criterios de aceitacao:**
- SoC e tensao legiveis e estaveis (sem spikes > 5% entre leituras consecutivas)
- EVT_POWER_LOW publicado ao cruzar threshold
- Brownout reason detectado e logado em teste com resistor de carga
- Estado de charger transiciona corretamente ao conectar/desconectar USB

**Depende de hardware real:** Sim — MAX17048 e bq25185 fisicos

---

### ETAPA 1.1 — Display ST7789

**Objetivo:** Feedback visual disponivel para diagnotico e estado do sistema.

**Por que aqui:** Zero risco eletrico, zero risco mecanico. Muito valor de debug — substitui UART puro como output de estado. Todas as fases seguintes se beneficiam de ter display.

**Escopo que entra:**
- Driver SPI para ST7789: init, fill, draw retangulo, draw texto (fonte bitmap embutida)
- Framebuffer em PSRAM (240x320 @ 16bpp = 150KB)
- DMA para transferencia SPI
- `services/display_service.h/c`: API de alto nivel, telas de diagnostico
- Telas minimas: BOOTING, ERROR(code, msg), POWER(soc%, voltage, charger_status), SAFE_MODE
- DisplayTask: 30fps maximo, sem bloquear tasks de maior prioridade

**Fora do escopo:**
- LVGL ou qualquer framework de UI completo
- Animacoes expressivas
- Conteudo de comportamento/persona

**Riscos:**
- Pressao de PSRAM: framebuffer deve vir de PSRAM, nao SRAM interna
- SPI clock alto com cabo longo pode causar glitches → testar em 40MHz antes de 80MHz
- DMA transfer longo bloqueia SPI bus → nao compartilhar bus com SD card na mesma SPI durante transfer

**Criterios de aceitacao:**
- Display inicializa sem artifacts visuais
- Tela BOOTING visivel em < 2s apos power-on
- Tela ERROR exibe codigo e mensagem legivel
- DMA transfer nao bloqueia CPU por mais de 5ms por frame

---

### ETAPA 1.2 — LEDs WS2812

**Objetivo:** Indicacao de estado por LED RGB usavel como debug visual de eventos.

**Escopo que entra:**
- Driver via RMT: sequencia de cor para 2 LEDs, sem bloquear CPU
- `services/led_service.h/c`: estados mapeados para padrões
  - BOOTING: azul pulsante
  - OK: verde solido
  - ERROR: vermelho fixo
  - LOW_BATTERY: amarelo
  - CRITICAL: vermelho piscante rapido
  - SAFE_MODE: roxo piscante lento
  - CHARGING: azul piscante lento
- Assinatura de eventos de power e system para mudar estado automaticamente

**Riscos:**
- RMT interrompido durante sequencia pode causar glitch → canal dedicado, nao interromper
- Nivel logico 3.3V pode ser marginal para alguns WS2812B → testar; se houver glitch, adicionar level shifter

**Criterios de aceitacao:**
- Cor muda corretamente ao receber evento do event bus
- Nenhum glitch visual em operacao normal
- SAFE_MODE visualmente distinto de BOOTING e OK

---

### ETAPA 1.3 — Storage (microSD)

**Objetivo:** Armazenamento persistente para logs, assets e calibracoes.

> Ver adendo completo em `docs/PERSISTENCE.md`. O microSD nao e periferico opcional — e camada central de persistencia do sistema.

**Escopo que entra:**
- Init microSD via SPI com FAT32/VFS do ESP-IDF
- `services/storage_service.h/c`: API sobre VFS — open, read, write, append, sync, close
- Log rotation: logger redireciona para arquivo no SD com limite de tamanho (ex: 1MB por arquivo, max 5 arquivos)
- Diagnostico: deteccao de ausencia de card, erro de mount, espaco disponivel
- Fallback: se microSD ausente ou com erro, operar sem storage (log apenas UART), publicar EVT_SERVICE_FAILED

**Riscos:**
- SPI compartilhado com display: CS separados e arbitragem correta
- Cards baratos tem latencia de write inconsistente → ring buffer assincrono para log
- Remocao a quente sem unmount corrompe FAT32 → documentar como risco nao tratado nesta fase

**Criterios de aceitacao:**
- Log gravado em arquivo sem perda apos reset controlado
- Deteccao correta de ausencia de card (sem crash, modo degradado logado)
- Write de log nao bloqueia LoggerTask por mais de 20ms

---

### ETAPA 1.3b — PersistenceManager e Estrutura de Memoria

**Objetivo:** Estabelecer a camada de persistencia que sustentara a memoria de longo prazo do robo, com API completa e estrutura de diretorios no SD, mesmo que a maioria das funcionalidades esteja vazia inicialmente.

**Por que nao pode ser adiado para a fase de comportamento:** O BehaviorFSM (Etapa 7.1) e o primeiro consumidor da memoria. Se a API e os schemas nao existirem antes dele, o comportamento crescera sem memoria e precisara ser refatorado — exatamente o remendo arquitetural que se quer evitar.

**Escopo que entra:**
- Criacao da estrutura de diretorios `/nodebot/` no SD na primeira inicializacao
- Health check file: `/nodebot/.health` escrito e lido no boot para verificar SD funcional
- `infra/persistence_manager.h/c`: API completa com stubs funcionais
- Structs definidas e versionadas em `infra/nb_persist_types.h`: `nb_episode_t`, `nb_preferences_t`, `nb_persona_traits_t`, `nb_context_t`, `nb_system_snapshot_t`
- Snapshot de sistema completamente implementado (simples, alto valor imediato de diagnostico)
- SD health monitor task (verifica SD a cada 60s, publica EVT_STORAGE_DEGRADED se falhar)
- Evento `EVT_STORAGE_DEGRADED` implementado no event bus

**Fora do escopo:**
- Leitura e escrita de preferencias (sem comportamento para consumir ainda)
- Registros episodicos (sem BehaviorFSM para gerar)
- Evolucao de persona traits (sem interacao real)

**Entregaveis:**
- `infra/persistence_manager.h` com API completa e contratos documentados
- `infra/persistence_manager.c` com snapshots funcionais e stubs para memoria longa
- `infra/nb_persist_types.h` com schemas versionados de todos os tipos de dados
- Estrutura `/nodebot/` criada no SD com subdiretorios e arquivo `.health`

**Criterios de aceitacao:**
- Boot com SD presente: estrutura de diretorios criada, health check OK, snapshot gravado em `/nodebot/memory/snapshots/`
- Boot sem SD: modo amnesico ativo, log em UART, nenhum crash
- Remocao de SD em operacao: EVT_STORAGE_DEGRADED publicado em < 120s, sistema continua operando normalmente
- Snapshot carregado no proximo boot e logado (uptime, soc_pct, boot_count do boot anterior)

---

### ETAPA 2.1 — Validacao de Power Path (medicoes fisicas)

**Objetivo:** Caracterizar completamente o sistema de energia com medicoes reais antes de qualquer atuador ou periferico pesado.

**Por que e obrigatoria antes dos servos:** Um power path nao validado causa classes inteiras de bugs que parecem ser de software mas sao de hardware. Esta etapa nao e opcional.

**Escopo — medicoes com multimetro e osciloscópio:**
- Tensao e ripple no barramento 3.3V sob carga do sistema base (ESP32 + display + LEDs)
- Tensao no boost 5V sem carga e com carga resistiva simulada
- Ripple no 5V do boost sob transientes de carga (< 50mV = OK)
- Corrente total do sistema base medida
- Brownout simulado: reduzir tensao artificialmente e verificar comportamento
- Carregamento: conectar USB, verificar transicao BULK → ABSORPTION → FLOAT via I2C
- Fuel gauge: descarregar parcialmente, recarregar, verificar rastreamento do MAX17048

**Entregaveis (nao codigo — documentos de medicao):**
- Planilha/documento com valores medidos de: Vout_3V3, Vout_5V, ripple_5V, I_total_base
- Thresholds de brownout ajustados com base em medicoes reais
- Decisao documentada: boost e estavel o suficiente para servos? (criteria: ripple < 50mV, Vout = 5V ± 5% sob 3A)

**Depende de hardware real:** 100%. Nao pode ser simulado.

**Sinais de imaturidade:**
- Nunca ter medido ripple no 5V com osciloscópio
- Nao saber a corrente maxima do sistema base
- Nunca ter testado brownout deliberadamente no hardware

---

### ETAPA 3.1 — Servo Driver (comunicacao e status)

**Objetivo:** Comunicacao confiavel com SCS0009 via FE-TTLinker — SEM nenhum movimento.

**Por que separado do movimento:** Protocolo de comunicacao deve ser estavel e status legivel antes de qualquer comando de posicao. Enviar posicao para servo sem saber o estado dele e perigoso.

**Escopo que entra:**
- Driver UART half-duplex para protocolo SCServo via FE-TTLinker
- Operacoes: PING, READ_REGISTER, WRITE_REGISTER (somente parametros nao-movimento)
- Leitura de: posicao atual, temperatura, load, voltage do servo, error flags
- Leitura de configuracao: limites de posicao configurados no servo, limites de temperatura
- Log de estado de ambos os servos no boot
- Nenhum comando de posicao implementado ainda

**Riscos:**
- FE-TTLinker pode ter latencia variavel → medir e definir timeout minimo razoavel (ex: 5ms)
- Half-duplex requer switching correto de TX/RX direction → bug aqui causa colisao silenciosa

**Criterios de aceitacao:**
- PING responde de ambos os servos com ID correto
- Temperatura lida plausivel (temperatura ambiente ≈ 25-35°C sem carga)
- Posicao atual lida estavelmente (sem bits de ruido ou valores erraticos)
- Error flags = 0 em condicao de repouso

---

### ETAPA 3.2 — Servo Motion Safety Layer

**Objetivo:** Implementar todas as protecoes arquiteturais que tornam movimento seguro — ANTES de mover qualquer servo.

**Escopo que entra:**
- Limites de posicao por servo (min_pos, max_pos) verificados em software antes de qualquer comando
- Limite de temperatura: se > 70°C → desabilitar servo, publicar EVT_SERVO_OVERTEMP
- Limite de load: se load > threshold por N leituras consecutivas → stall detectado, publicar EVT_SERVO_STALL
- Heartbeat de movimento: se MotionService nao enviar comando em 500ms → torque-off mode
- Rampa de aceleracao: nenhum step direto entre posicoes distantes — interpolacao linear minima
- Teste de movimento minimo: mover 5° da posicao atual, verificar resposta, retornar
- Estados de servo: UNINITIALIZED → IDLE → MOVING → FAULT
- Saida de FAULT requer reset explicito, nao automatico

**Ver `docs/SERVO_SAFETY.md` para detalhamento completo.**

**Criterios de aceitacao (todos obrigatorios):**
1. Comando fora dos limites e rejeitado (sem envio ao servo, sem crash, apenas log)
2. Temperatura alta desabilita servo e publica evento
3. Stall detectado e servo para apos N leituras de load alto
4. Heartbeat ausente por 500ms entra em torque-off
5. Teste de movimento minimo (5°) passa em ambos os servos
6. Movimento brusco (step grande) e substituido por rampa

---

### ETAPA 4.1 — Touch Sensor

**Objetivo:** Deteccao confiavel de toque via fita de cobre com denoising.

**Escopo que entra:**
- Driver para periférico touch interno do ESP32-S3
- Calibracao de baseline — persistida em NVS via ConfigManager
- Filtro de debounce e histerese
- `services/touch_service.h/c`: publicacao de EVT_TOUCH_START, EVT_TOUCH_END, EVT_TOUCH_HOLD
- Re-calibracao manual triggeravel via comando serial

**Riscos:**
- Capacitancia da fita varia com temperatura e umidade → threshold deve suportar recalibracao
- Ruido eletrico de servos pode afetar leitura → testar com servos em movimento

**Criterios de aceitacao:**
- Touch detectado com latencia < 50ms
- Zero falsos positivos em 5 minutos sem toque
- Recalibracao funcional via serial

---

### ETAPA 4.2 — IMU (MPU-6050)

**Objetivo:** Leitura filtrada de aceleracao e giroscopio com deteccao de eventos criticos de seguranca.

**Escopo que entra:**
- Driver I2C para MPU-6050: init, leitura de acelerometro e giroscopio a 50Hz
- Filtro complementar basico para attitude estimation
- `services/imu_service.h/c`: publicacao de EVT_IMU_TILT, EVT_IMU_FALL
- Deteccao de queda livre (aceleracao < 0.3g por > 100ms) → publica EVT_IMU_FALL → MotionService reage desligando torque dos servos

**Riscos:**
- Vibracao dos servos satura IMU → filtrar com janela de media movel antes da deteccao de eventos

**Criterios de aceitacao:**
- Leitura estavel a 50Hz sem perdas de sample
- Deteccao de queda testada (segurar e soltar robo) → servos desligam
- Nenhum falso alarme de queda em 10 minutos de uso normal

---

### ETAPA 5.1 — Captura de Audio (INMP441)

**Objetivo:** Pipeline de captura via I2S funcionando de forma estavel.

**Escopo que entra:**
- Driver I2S0 para INMP441: 16kHz, 32-bit (16 efetivos), DMA buffer em SRAM interna
- Ring buffer de audio em PSRAM (ex: 2s de audio = 64KB @ 16kHz/16bit)
- `services/audio_capture_service.h/c`: AudioCaptureTask, publicacao de chunks de audio
- VAD simples por energia RMS: publicar EVT_VOICE_DETECTED quando nivel > threshold

**Riscos:**
- DMA I2S0 e DMA de camera podem conflitar → testar camera + audio juntos antes de integrar comportamento
- PSRAM ring buffer: alocacao falha se heap fragmentado → verificar heap antes de alocar

**Criterios de aceitacao:**
- Captura continua sem drop de samples por 5 minutos
- VAD detecta fala e silencio corretamente em ambiente de escritorio
- Nenhum conflito de DMA quando operando com display ativo

---

### ETAPA 5.2 — Playback de Audio (MAX98357A)

**Objetivo:** Reproducao de arquivos de audio do microSD.

**Escopo que entra:**
- Driver I2S1 para MAX98357A: sample rate configuravel, DMA
- `services/audio_playback_service.h/c`: fila de reproducao, controle de volume via GAIN pin
- Decoder WAV PCM 16-bit (sem decoder complexo por ora)
- Verificacao de energia antes de playback: nao reproduzir se SoC < 10%

**Fora do escopo:**
- TTS (sintese de voz)
- MP3 decoder
- Streaming remoto

**Criterios de aceitacao:**
- Arquivo WAV 16kHz/16bit reproduzido sem distorcao
- Fila funciona para multiplos arquivos em sequencia
- Nenhum conflito de I2S com INMP441 (I2S0 e I2S1 simultaneos)

---

### ETAPA 6.1 — Camera OV2640

**Objetivo:** Captura de frames JPEG funcional com gestao de memoria segura.

**Por que tao tarde:** OV2640 e o maior consumidor de PSRAM. Deve entrar somente apos toda a gestao de memoria estar estabelecida pelos outros servicos. Integrar cedo gera conflitos de heap impossiveis de depurar.

**Escopo que entra:**
- Driver via `esp_camera` (componente oficial do ESP-IDF)
- Resolucao inicial: QVGA (320x240) para minimizar pressao de memoria
- Captura sob demanda (nao streaming continuo)
- `services/camera_service.h/c`: request/release de frame com timeout
- Frame buffer em PSRAM, liberado imediatamente apos uso
- Verificacao de heap: nao inicializar camera se heap PSRAM < 500KB

**Riscos:**
- `esp_camera` consome 200-400KB de PSRAM por frame buffer
- Camera + audio simultâneos podem saturar DMA → testar explicitamente
- DVP trava ~12 pinos de GPIO permanentemente

**Criterios de aceitacao:**
- Frame QVGA capturado sem artifacts
- Heap livre > 40KB de SRAM interna com camera ativa
- Camera + audio simultaneos sem drop de samples de audio

---

### ETAPA 7.1 — Comportamento e Persona

**Objetivo:** Integrar subsistemas em comportamentos coerentes com personalidade do robo.

**Pre-requisito absoluto:** Todas as etapas anteriores concluidas e com criterios de aceitacao atendidos.

**Escopo que entra:**
- FSM de estado do robo: IDLE, ALERT, LISTENING, SPEAKING, MOVING, LOW_POWER, ERROR, SAFE_MODE
- Reacoes a eventos: EVT_TOUCH_START → expressao, EVT_VOICE_DETECTED → LISTENING, EVT_IMU_FALL → parar movimento
- Orquestracao de servicos em sincronia (LED + display + servo + audio)
- Politica de prioridade de comportamentos conflitantes
- Respeitar estado de energia: comportamentos reduzidos em LOW_POWER

---

### ETAPA 8.1 — Integracao Total e Validacao Longa

**Objetivo:** Validar estabilidade do sistema completo sob condicoes reais e prolongadas.

**Escopo que entra:**
- Soak test de 24h com todos os subsistemas ativos
- Teste de autonomia de bateria (tempo real de operacao)
- Stress test: multiplos eventos simultâneos por 30 minutos
- Regressao: verificar que criterios de aceitacao das etapas anteriores ainda passam com sistema completo
- Medicao de consumo de CPU e memoria em operacao real
- Refinamento de thresholds de energia, temperatura e stall com dados reais

---

## Definicao de "Base Solida Concluida"

A fundacao pode ser declarada suficientemente robusta para integracao de servicos avancados quando **todos** os criterios abaixo forem atendidos:

### Criterios Tecnicos
- Boot completo e observavel em < 3s
- Boot report disponivel no event bus: reset reason, boot count, safe mode flag, power state
- ConfigManager operacional com NVS, defaults e migracao testados
- Event bus operacional sem bloqueio indefinido
- Logger funcionando em UART + microSD com rotacao automatica
- PowerManager lendo MAX17048 e bq25185 continuamente
- Politica de acao em LOW e CRITICAL battery implementada e testada
- Display mostrando estado de energia e sistema

### Criterios de Estabilidade
- 24 horas ininterruptas sem crash, leak ou comportamento anomalo
- Watchdog disparou e recuperou corretamente em teste deliberado
- Brownout simulado gerou log de motivo correto
- Safe mode ativou corretamente apos 3 crashes simulados
- Nenhum stack overflow detectado em 24h

### Criterios de Observabilidade
- Qualquer falha de periferico durante boot logada com modulo, codigo e acao
- Estado de energia visivel em display e LEDs
- Task monitor: uso de stack e CPU logado a cada 60s
- Heap monitor: alerta se SRAM < 20KB livre

### Criterios de Seguranca
- Nenhum servico avancado inicializa com bateria CRITICAL
- Brownout threshold configurado e testado com hardware real
- Politica de erro documentada e implementada para todos os modulos de infra

### Criterios de Teste
- Testes unitarios: ConfigManager (read/write/default/migrate), EventBus (publish/subscribe/timeout), BootManager (reset reason, safe mode)
- Teste de integracao: boot completo 5x consecutivos sem falha

### Criterios Arquiteturais
- Camadas explicitas sem vazamento (BSP → HAL → Infra → Services → App)
- Nenhum modulo acessando internals de outro servico diretamente
- Headers de API publica documentados com contrato de cada funcao
