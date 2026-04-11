# NodeBot — Servos e Motion Safety

## Por que servos nao devem entrar cedo na integracao

Os SCS0009 sao atuadores fisicos com energia armazenada no boost 5V/10A, operando sobre mecanismo com resistencia mecanica real. Os riscos nao sao abstratos:

| Cenario de risco            | Consequencia                                                    |
|-----------------------------|-----------------------------------------------------------------|
| Stall sem deteccao          | Corrente maxima por segundos → superaquecimento → queima        |
| Posicao fora do limite mec. | Quebra de peca plastica, forcamento de estrutura               |
| Pico de corrente em aceler. | Brownout do sistema inteiro (pode corromper dados)              |
| Firmware travado + torque   | Servo em torque ativo indefinidamente sem resposta              |
| Temperatura nao monitorada  | Dano termico ao servo e ao mecanismo adjacente                  |

A integracao com comportamento (persona, expressividade, reacao a eventos) deve acontecer somente depois que a safety layer esta implementada, testada e validada independentemente.

---

## Caracteristicas dos SCS0009 (relevantes para safety)

- **Protocolo:** SCServo bus serial half-duplex, 1 Mbps via FE-TTLinker → UART ESP32-S3
- **Alimentacao:** 4.5V - 6V (via TPS61088 boost 5V)
- **Corrente nominal:** ~200mA em movimento sem carga
- **Corrente de stall:** ~1.5A (podendo chegar a 2A em versoes com torque alto)
- **Range de posicao:** 0 - 1023 (0° a 300° tipicamente, depende da montagem mecanica)
- **Registradores de seguranca lidos por software:**
  - `Current Position` (0x38): posicao atual em steps
  - `Present Temperature` (0x3F): temperatura interna em °C
  - `Present Load` (0x40): carga/torque atual (0-100%)
  - `Present Voltage` (0x42): tensao de alimentacao
  - `Hardware Error Status` (0x3A): bits de erro de hardware

---

## Pre-Requisitos Antes de Qualquer Movimento

### Pre-Requisitos de Sistema

- [ ] Power path validado (Etapa 2.1 concluida com todos os criterios aprovados)
- [ ] Corrente de pico de 2 servos em movimento medida fisicamente
- [ ] Tensao do boost estavel durante movimento (Vout > 4.5V sob carga medida)
- [ ] Brownout threshold configurado acima da tensao minima do sistema sob carga de servo
- [ ] IMU ativo e deteccao de queda funcionando (EVT_IMU_FALL desliga torque dos servos)

### Pre-Requisitos de Software

- [ ] ServoDriver estavel com PING e READ_STATUS funcionando (Etapa 3.1 concluida)
- [ ] Motion Safety Layer implementada e todos os 6 testes de seguranca aprovados
- [ ] WatchdogService com heartbeat de movimento configurado
- [ ] Limites mecanicos fisicos documentados e mapeados para limites de software
- [ ] Rampa de aceleracao implementada (nenhum step direto entre posicoes distantes)

### Pre-Requisitos de Hardware

- [ ] Limites mecanicos fisicos verificados manualmente (sem colisao no range de movimento)
- [ ] Cabo de alimentacao dos servos com capacitor de bypass (100µF + 100nF) na entrada do FE-TTLinker para absorver picos
- [ ] FE-TTLinker conectado e comunicacao testada em Etapa 3.1

---

## Arquitetura da Safety Layer

Nenhum caminho pode bypass a SafetyLayer. Ela nao tem modo de override por comportamento de alto nivel.

```
[BehaviorFSM / Aplicacao]
         |
         | solicita movimento (ID, posicao_alvo, velocidade)
         v
[MotionService]
  - verifica estado atual do servo (IDLE? FAULT?)
  - verifica estado de energia (CRITICAL? → rejeita)
  - verifica temperatura atual (> 70°C? → rejeita)
  - verifica load atual (stall detectado? → rejeita)
         |
         | aprovado
         v
[ServoSafetyLayer]
  - aplica hard limits de posicao (min_pos, max_pos) — NAO configuravel por comportamento
  - clipa posicao ao limite se necessario (nao rejeita — clipa e loga)
  - aplica rampa de aceleracao (interpolacao linear de posicao atual → alvo)
  - nao permite step > MAX_STEP_PER_TICK (configuravel, default: 20 steps por 10ms)
         |
         | comando filtrado e seguro
         v
[ServoDriver]
  - transmite comando SCServo via FE-TTLinker UART half-duplex
  - aguarda resposta (timeout: 5ms)
  - le posicao de volta para verificar chegada
         |
         | feedback de posicao, temperatura, load
         v
[MotionService]
  - atualiza estado interno
  - publica EVT_MOTION_COMPLETE ou EVT_SERVO_STALL / EVT_SERVO_OVERTEMP
```

---

## Limites e Thresholds de Safety

Todos os thresholds sao configurados via ConfigManager e persistidos em NVS. Nenhum pode ser alterado em runtime por servico de comportamento.

| Parametro              | Default  | Chave NVS               | Descricao                              |
|------------------------|----------|-------------------------|----------------------------------------|
| SERVO0_MIN_POS         | 100      | nb_servo/s0_min         | Posicao minima servo 0 em steps       |
| SERVO0_MAX_POS         | 923      | nb_servo/s0_max         | Posicao maxima servo 0 em steps       |
| SERVO1_MIN_POS         | 100      | nb_servo/s1_min         | Posicao minima servo 1 em steps       |
| SERVO1_MAX_POS         | 923      | nb_servo/s1_max         | Posicao maxima servo 1 em steps       |
| OVERTEMP_THRESHOLD_C   | 70       | nb_servo/max_temp       | Temperatura de desligamento (°C)       |
| OVERLOAD_THRESHOLD_PCT | 80       | nb_servo/max_load       | Load % para deteccao de stall         |
| STALL_CONSEC_READINGS  | 5        | nb_servo/stall_n        | Leituras consecutivas para confirmar stall |
| HEARTBEAT_TIMEOUT_MS   | 500      | nb_servo/hb_ms          | Timeout de heartbeat → torque-off     |
| MAX_STEP_PER_TICK      | 20       | nb_servo/max_step       | Passo maximo por tick de 10ms         |

> Os limites de posicao (min/max) devem ser calibrados fisicamente na montagem real do robo. Os defaults acima sao conservadores mas devem ser ajustados com base nos limites mecanicos reais.

---

## Estados do Servo

```
UNINITIALIZED
     |
     | ping bem-sucedido + status OK
     v
IDLE ←───────────────────────────────────────────┐
     |                                            |
     | comando de posicao recebido                | motion_complete ou torque-off
     v                                            |
MOVING ──────────────────────────────────────────┘
     |
     | temperatura > threshold   OU
     | stall detectado (N leituras de load alto)  OU
     | heartbeat ausente > 500ms                  OU
     | hardware error flag no servo
     v
FAULT
     |
     | reset explicito via serial OU comando de manutencao
     v
IDLE
```

Saida de FAULT **nunca e automatica**. Exige intervencao explicita.

---

## Heartbeat de Movimento

O MotionService deve enviar heartbeat ao WatchdogService a cada ciclo de 10ms enquanto servos estao habilitados. Se o heartbeat parar (task travada, deadlock, watchdog de software):

1. ServoSafetyLayer detecta ausencia de heartbeat apos 500ms
2. Envia comando `TORQUE_OFF` para ambos os servos imediatamente
3. Publica EVT_MOTION_FAULT
4. Estado dos servos → FAULT

Isso garante que um firmware travado nao deixa servos em torque ativo.

---

## Testes Minimos de Seguranca

Todos os 6 testes abaixo sao obrigatorios antes de considerar servos "seguros para uso progressivo":

### Teste 1 — Limite de posicao

**Procedimento:** Enviar comando com posicao > MAX_POS para servo 0.
**Criterio:** Comando clipado para MAX_POS e logado. Servo nao recebe posicao invalida. Nenhum crash.

### Teste 2 — Stall simulado

**Procedimento:** Segurar servo manualmente enquanto MotionService tenta mover. Aguardar N leituras de load alto.
**Criterio:** EVT_SERVO_STALL publicado. Servo para. Estado → FAULT. Log com numero de leituras e valor de load.

### Teste 3 — Temperatura alta

**Procedimento:** Injetar valor de temperatura > OVERTEMP_THRESHOLD via registro de teste (ou esperar aquecimento real).
**Criterio:** EVT_SERVO_OVERTEMP publicado. Torque desligado. Estado → FAULT. Log com temperatura medida.

### Teste 4 — Heartbeat ausente

**Procedimento:** Suspender MotionService task por 1 segundo (via debug command).
**Criterio:** Apos 500ms, torque-off em ambos os servos. EVT_MOTION_FAULT publicado.

### Teste 5 — Brownout durante movimento

**Procedimento:** Simular carga pesada durante movimento (resistor de carga no 5V) e verificar que sistema nao colapse.
**Criterio:** Tensao do boost permanece > 4.5V durante movimento. Brownout nao dispara.

### Teste 6 — Movimento progressivo minimo

**Procedimento:** Comandar movimento de 5° (pequeno step), verificar posicao reportada pelo servo, retornar.
**Criterio:** Posicao reportada muda coerentemente (dentro de ±5 steps do alvo). Nenhum erro de comunicacao.

---

## O que NAO Fazer

- Nao enviar posicao aleatoria sem verificar estado atual do servo
- Nao mover dois servos para posicoes extremas simultaneamente no primeiro teste
- Nao desabilitar safety layer "temporariamente" para debug — usar limites mais amplos configurados corretamente
- Nao ignorar EVT_SERVO_OVERTEMP — sempre aguardar servo esfriar antes de reativar
- Nao implementar override de heartbeat timeout por "conveniencia" de comportamento
