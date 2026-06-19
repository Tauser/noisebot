# NoiseBot — Servo Safety

## Princípio

Motion é uma área crítica de segurança física. Um servo que trava, que excede limites mecânicos ou que recebe comando incorreto pode danificar permanentemente a estrutura do robot. Não há recuperação sem intervenção manual.

**Regra:** Nenhum código de movimento expressivo (Etapa 3.3 em diante) é escrito ou testado antes de todos os critérios da Etapa 3.2 (safety layer) serem verificados com hardware real.

---

## Parâmetros de Safety

Valores iniciais conservadores. **Expandir apenas após calibração mecânica com hardware real.**

### Identificadores

```c
#define NB_SERVO_NECK_PAN   1  // Rotação horizontal (esquerda/direita)
#define NB_SERVO_NECK_TILT  2  // Inclinação vertical (cima/baixo)
```

### Limites de Posição (unidades SCS: 0–1023, onde 512 ≈ 0°)

O SCS0009 usa ADC de 10 bits — range 0..1023 (não 0..4095 do STS series).
Range mecânico de 300°. O range seguro é muito menor:

```
Conversão: 1° ≈ 1023/300 ≈ 3.41 unidades SCS
Centro: 512 unidades

PAN (horizontal):
  Min: 410 unidades (centro − 30° = 512 − 102)
  Max: 614 unidades (centro + 30° = 512 + 102)
  Range seguro: 60° total

TILT (vertical):
  Min: 461 unidades (centro − 15° = 512 − 51)
  Max: 563 unidades (centro + 15° = 512 + 51)
  Range seguro: 30° total (mecânica mais restrita no eixo vertical)
```

> **Validado em bancada (maio 2026):** READ_POSITION com servo parado retornou
> valores ≤1023. Escala 0..4095 é do STS series — não se aplica ao SCS0009.

**Limites de hardware EEPROM (confirmados via FT SCServo Debug, maio 2026):**

| Registro EEPROM | Nome | Valor confirmado |
|---|---|---|
| 9 | Min Position Limit | **20** (não 0) |
| 11 | Max Position Limit | **1003** (não 1023) |
| 13 | Max Temperature Limit | 80°C |
| 16 | Max Torque Limit | 1000 |
| 26 | CW Dead Band | 10 steps |
| 27 | CCW Dead Band | 10 steps |

Os limites EEPROM são hardware: o servo rejeita comandos fora de 20..1003 mesmo que o software envie valores extremos. Os limites de software acima (410..614 PAN, 461..563 TILT) ficam confortavelmente dentro desse range.

O ADC continua lendo 0..1023 (posição física real) independente dos limites EEPROM — estes só afetam Goal Position.

**Importante:** Estes limites são configuráveis via NVS (`nb_cfg::servo_pan_min`, etc.). Os defaults acima são conservadores. Após montagem e teste mecânico, expandir gradualmente com observação visual a cada etapa.

### Limites de Velocidade (unidades SCS: 0–1023)

```c
#define NB_SERVO_SPEED_MAX_SAFE     200  // Movimento expressivo normal
#define NB_SERVO_SPEED_MAX_FAST     400  // Saccades rápidos de gaze
#define NB_SERVO_SPEED_ABSOLUTE_MAX 600  // Nunca ultrapassar em nenhum contexto
```

### Proteção de Stall (sobrecorrente)

Load do servo = indicador de carga mecânica (0–1000 unidades SCS ≈ 0–100%).

```c
#define NB_SERVO_LOAD_WARN_PCT     40   // % → publicar NB_EVT_SERVO_STALL_DETECTED
#define NB_SERVO_LOAD_CRITICAL_PCT 70   // % → desabilitar torque imediatamente
#define NB_SERVO_STALL_TIME_MS    100   // ms em load crítico antes de disable
```

### Temperatura

```c
#define NB_SERVO_TEMP_WARN_C     55   // °C → NB_EVT_SERVO_TEMP_WARN
#define NB_SERVO_TEMP_CRITICAL_C 70   // °C → disable torque, NB_EVT_SERVO_TEMP_CRITICAL
```

### Heartbeat

```c
#define NB_MOTION_HEARTBEAT_PERIOD_MS  200  // motion_task publica a cada 200ms
#define NB_MOTION_HEARTBEAT_TIMEOUT_MS 600  // safety_task espera max 600ms
```

---

## Comportamento do Sistema em Falha

### Sequência de Disable de Torque

Ao detectar condição de falha (stall, temperatura, heartbeat timeout, brownout):

1. Tentar enviar posição de parking (centro) ao servo com velocidade baixa — timeout 50ms
2. Enviar comando TORQUE_DISABLE ao servo
3. Publicar evento correspondente no event bus
4. Registrar no log: timestamp, servo_id, motivo, posição no momento
5. Transitar estado de motion_safety para FAULT
6. Aguardar reset explícito (não auto-recover)

**Em brownout:** Pular etapa 1 (sem tempo). Ir direto para TORQUE_DISABLE.

### Estado FAULT

No estado FAULT, `motion_safety` rejeita qualquer comando de movimento.
O sistema continua operando normalmente em todos os outros subsistemas.
Para sair do FAULT: reset explícito via `motion_safety_reset_fault()` (requer confirmação de operador).

---

## Monitoramento Contínuo

`nb_servo_safety_task` (Core 1, prioridade 23) executa a 20Hz:

```
A cada 50ms:
1. Ler load de servo PAN
2. Ler load de servo TILT
3. Se load > WARN: publicar NB_EVT_SERVO_STALL_DETECTED
4. Se load > CRITICAL por > STALL_TIME_MS: disable (ver sequência acima)
5. Ler temperatura (a cada 500ms — não 50ms, para não sobrecarregar o barramento)
6. Verificar heartbeat da motion_task
7. Reportar ao TWDT (watchdog da própria task)
```

---

## Protocolo de Liberação

Este checklist deve ser executado com hardware real antes de qualquer feature de movimento expressivo ser desenvolvida:

**Fase 1 — Comunicação (Etapa 3.1)**

- [ ] PING servo ID=1: OK
- [ ] PING servo ID=2: OK
- [ ] READ posição de ambos os servos: valores estáveis
- [ ] READ temperatura: valores plausíveis (20–40°C)
- [ ] Timeout de comunicação: retorna erro, não trava

**Fase 2 — Safety Layer (Etapa 3.2)**

- [ ] Comando de posição abaixo do mínimo: rejeitado pelo software, servo não se move
- [ ] Comando de posição acima do máximo: rejeitado, servo não se move
- [ ] Stall simulado (bloquear servo com a mão em movimento lento): torque disable em <150ms
- [ ] Heartbeat timeout simulado (matar motion_task): disable em <600ms
- [ ] Brownout simulado durante movimento: disable antes do reset
- [ ] Temperatura crítica simulada (override de software): disable, evento publicado
- [ ] 10 minutos de operação idle (sem movimento): temperatura estável, sem alarme falso

**Fase 3 — Primitivos (Etapa 3.3)**

- [ ] Pan: mover de centro para mínimo (30° esquerda), retornar. 50 vezes. Sem desvio >2°.
- [ ] Pan: mover de centro para máximo (30° direita), retornar. 50 vezes.
- [ ] Tilt: mover de centro para mínimo (15° baixo), retornar. 50 vezes.
- [ ] Tilt: mover de centro para máximo (15° cima), retornar. 50 vezes.
- [ ] Movimento simultâneo PAN + TILT: sem interferência mecânica
- [ ] Após 100 ciclos: temperatura dos servos abaixo de TEMP_WARN

**SOMENTE após todos os itens acima verificados:** iniciar desenvolvimento de gaze, idle e comportamento expressivo.

---

## Gate de Reintegração — Pré-condições para Conectar os Servos

Este checklist deve estar **100% verde** antes de plugar qualquer servo na placa.
Cada item tem campo de medição — preencher com evidência real antes de marcar.

### Checklist (F44)

**G1 — Races de arm() corrigidas (F03)**
- [ ] `arm_cancel()` em todos os caminhos de erro de `motion_safety_arm()`
- [ ] Guarda `s_state == INITIALIZING` antes de setar ARMED
- [ ] Teste de concorrência host: fault/brownout em todos os pontos de interleaving → ARMED nunca sucede um fault
- Evidência: _______________

**G2 — Latência de emergência medida (F05)**
- [ ] `park_and_disable()` sem `write_position` (só torque-off)
- [ ] Medição com `esp_timer`: fault→último byte do torque-off no pior caso (safety_task no meio de retry)
- [ ] p99 < 150 ms confirmado
- Medição: _______________ ms (p99)

**G3 — Veto de safety no choke point (F07)**
- [ ] `servo_hal_write.h` presente; protótipos de escrita removidos de `servo_hal.h`
- [ ] busca por `servo_hal_write.h` em `firmware/main-controller/components/` retorna apenas `motion_service.c` e `motion_safety.c`
- Evidência: _______________

**G4 — EMI do bus mensurável (F08)**
- [ ] Contadores `s_bus_timeouts`/`s_bus_errors` instrumentados no servo_hal
- [ ] `diagnostics_dump_to_sd()` inclui bus stats
- [ ] Taxa de erro com WiFi ativo + tráfego < 1% sustentada por 1 h
- Medição: _______________ % erro (WiFi ativo, 1 h)

**G5 — servo_test removido de main (F01)**
- [ ] `servo_test.h` não incluído em `firmware/main-controller/main/main.c`
- [ ] Build limpo sem tráfego no UART1 no boot (log de TX vazio)
- Evidência: _______________

**G6 — Calibração HTTP protegida (F02)**
- [ ] Handler de calibração HTTP atrás de `#if CONFIG_NB_CALIB_HTTP` (default n) ou retorna 403
- [ ] POST de calibração sem servos conectados retorna 403/404
- Evidência: _______________

**Data de verificação:** _______________
**Responsável:** _______________

---

## Notas de Montagem Mecânica

**Calibração de centro:**
A posição "centro" do servo (2048 unidades SCS) deve corresponder à posição visual de centro da cabeça do robot. Após montar fisicamente, verificar o centro real e ajustar `servo_pan_center` e `servo_tilt_center` no NVS se necessário.

**Expansão de range:**
Após calibrar o centro, expandir os limites gradualmente:

1. Mover até o limite atual e observar visualmente: há folga mecânica?
2. Se sim, aumentar o limite em 5° e repetir
3. Nunca expandir além do ponto onde a mecânica começa a se estressar

**Torque de idle:**
Em posição estacionária, o servo mantém torque habilitado para segurar a posição. Verificar se o torque de holding causa aquecimento excessivo em operação prolongada. Se sim, implementar torque pulsado (hold por 500ms, disable por 200ms — suficiente para posição estacionária sem carga).

---

## Referência de Protocolo SCS (Feetech SCSCL)

Formato de pacote:

```
[0xFF] [0xFF] [ID] [LENGTH] [INSTRUCTION] [PARAM_1] ... [PARAM_N] [CHECKSUM]
CHECKSUM = ~(ID + LENGTH + INSTRUCTION + PARAM_1 + ... + PARAM_N) & 0xFF
```

Instruções relevantes:
| Instrução | Código | Descrição |
|---|---|---|
| PING | 0x01 | Verificar comunicação com servo |
| READ | 0x02 | Ler registradores |
| WRITE | 0x03 | Escrever registradores |
| SYNC_WRITE | 0x83 | Escrever em múltiplos servos simultaneamente |

Registradores relevantes do SCS0009 (RAM — voláteis, perdem no power cycle):

> **ENDIANNESS — CRÍTICO:** O SCS0009 usa **big-endian** para valores de 16 bits:
> byte alto (H) no endereço menor, byte baixo (L) no endereço maior.
> As labels "L/H" da documentação Feetech referem-se ao endereço (menor/maior),
> **não** ao byte (baixo/alto). Confirmado via dump (maio 2026):
> `reg[0x38]=0x02 reg[0x39]=0x7A` → posição 634 = 0x027A (H=0x02 em 0x38, L=0x7A em 0x39).
>
> Encodings corretos no firmware:
> - **READ:**  `pos = ((uint16_t)reg[0x38] << 8) | reg[0x39]`
> - **WRITE:** `buf[0] = pos >> 8` (→ reg[0x2A] = H), `buf[1] = pos & 0xFF` (→ reg[0x2B] = L)

| Endereço | Label doc. | R/W | Byte real | Descrição |
|---|---|---|---|---|
| 0x28 (40) | Torque Enable | R/W | — | 1=torque ativo, 0=livre |
| 0x29 (41) | Acceleration | R/W | — | Rampa de aceleração |
| 0x2A (42) | Goal Position "L" | R/W | **H byte** | Posição destino — byte ALTO |
| 0x2B (43) | Goal Position "H" | R/W | **L byte** | Posição destino — byte BAIXO |
| 0x2C (44) | Goal Time "L" | R/W | **H byte** | Tempo de movimento — byte ALTO (ms) |
| 0x2D (45) | Goal Time "H" | R/W | **L byte** | Tempo de movimento — byte BAIXO |
| 0x2E (46) | Goal Speed "L" | R/W | **H byte** | Velocidade máxima — byte ALTO (0–1023) |
| 0x2F (47) | Goal Speed "H" | R/W | **L byte** | Velocidade máxima — byte BAIXO |
| 0x38 (56) | Present Position "L" | R | **H byte** | Posição atual — byte ALTO |
| 0x39 (57) | Present Position "H" | R | **L byte** | Posição atual — byte BAIXO |
| 0x3A (58) | Present Speed "L" | R | **H byte** | Velocidade atual — byte ALTO (bit15=direção) |
| 0x3B (59) | Present Speed "H" | R | **L byte** | Velocidade atual — byte BAIXO |
| 0x3C (60) | Present Load "L" | R | **H byte** | Carga atual — byte ALTO (0–1000) |
| 0x3D (61) | Present Load "H" | R | **L byte** | Carga atual — byte BAIXO |
| 0x3E (62) | Present Voltage | R | — | Tensão × 10 (ex: 0x32=50 → 5.0 V) |
| 0x3F (63) | Present Temperature | R | — | Temperatura em °C |

> Validado em bancada (maio 2026): endian confirmado via dump de registradores.
> Confirmar registradores EEPROM (0x00–0x17) com datasheet antes de escrever.
