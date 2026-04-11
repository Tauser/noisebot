# NodeBot — Servo Safety

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

### Limites de Posição (unidades SCS: 0–4095, onde 2048 ≈ 0°)

O SCS0009 tem range mecânico de 300° (0–4095). O range seguro é muito menor:

```
Conversão aproximada: 1° ≈ 13.65 unidades SCS
Centro: 2048 unidades

PAN (horizontal):
  Min: 1638 unidades (centro - 30°)
  Max: 2458 unidades (centro + 30°)
  Range seguro: 60° total

TILT (vertical):
  Min: 1843 unidades (centro - 15°)
  Max: 2253 unidades (centro + 15°)
  Range seguro: 30° total (mecânica mais restrita no eixo vertical)
```

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

Registradores relevantes do SCS0009:
| Endereço | Nome | R/W | Descrição |
|---|---|---|---|
| 0x38 (56) | Goal Position | R/W | Posição destino (0–4095) |
| 0x3A (58) | Moving Speed | R/W | Velocidade (0–1023) |
| 0x28 (40) | Present Position | R | Posição atual |
| 0x30 (48) | Present Load | R | Carga atual (indicador de torque) |
| 0x3F (63) | Present Temperature | R | Temperatura em °C |
| 0x28 (40) | Present Voltage | R+1 | Tensão de alimentação |
| 0x18 (24) | Torque Enable | R/W | 1=habilitado, 0=livre |

Confirmar endereços com datasheet físico do SCS0009 — podem variar entre revisões.
