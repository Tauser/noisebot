# NoiseBot — Energia e Alimentação

## Topologia (Fase Desktop)

```
Fonte Raspberry Pi 4 Official PSU
  5V DC / 3A / 15W
        │
        │ (conector dedicado, NÃO USB-C da placa)
        │
        ├──── ESP32-S3 board (5V in → LDO AMS1117 3.3V interno)
        │      └── ST7789 display (3.3V via placa)
        │      └── INMP441 mic (3.3V via placa)
        │      └── Touch (periférico interno, sem carga significativa)
        │
        ├──── WS2812 × 2 (5V direto)
        │
        ├──── MAX98357A amplificador (5V)
        │
        └──── Alimentação dedicada 6V/2A para servo rail
               │  (opção A: fonte separada 6V — mais simples para fase desktop)
               │  (opção B: boost robusto com layout próprio, ex: TPS61088)
               │  ⚠ Módulos boost baratos (MT3608, XL6009 clone) NÃO são confiáveis
               │    para servo: pico de corrente causa sag de tensão no output,
               │    anulando o benefício. Boost só funciona com componentes adequados
               │    (indutor baixo DCR, diodo Schottky rápido, cap de saída ≥220µF).
               │
               └──── FE-TTLinker Mini V2 (Vin: 6–6.5V)
                      │  SP6205M5-L-5-0 LDO interno: dropout ~0.8V
                      │  A 6V: Vout LDO ≈ 5.2V ✓ (dentro dos 4.8–6V do SCS0009)
                      │
                      └──[C1: 470µF/10V + C2: 100nF]──── SCS0009 × 2 (~5V)
                                                           ↑
                                            Capacitor bulk obrigatório
                                            (fisicamente próximo aos conectores dos servos)

       GND ────── GND comum (star ground)
                  Todos os componentes no mesmo ponto de referência
```

**Por que o boost é necessário:**
O FE-TTLinker Mini V2 usa o regulador SP6205M5-L-5-0 (LDO, 500mA) com dropout típico de ~0.8V. A 5V de entrada, o LDO fica em dropout e entrega apenas ~4.2V nos servos — abaixo do mínimo especificado de 4.8V. Confirmado pelo suporte Feetech (maio 2026). A 6V de entrada, o LDO entrega ~5.2V, dentro do range operacional dos SCS0009.

**Comparativo StackChan:** O StackChan não usa TTLinker externo. Tem rail de motor separado (VM EN) na PCB do corpo, controlado por PY32 IO Expander → MOSFET/load-switch dedicado. Hardware de servo projetado desde o início na placa. Não é comparável ao TTLinker solto — a função é equivalente, mas a implementação é integrada.

**Regra:** A USB-C da placa Freenove serve para programação e debug. **Não é a fonte de alimentação do sistema.** Alimentação do sistema entra por pino dedicado de 5V externo.

---

## Orçamento de Energia

| Componente                          | Típico    | Pico       | Notas                             |
| ----------------------------------- | --------- | ---------- | --------------------------------- |
| ESP32-S3 (CPU ativo + PSRAM)        | 300mW     | 500mW      | WiFi desabilitado                 |
| ST7789 + backlight (brightness 70%) | 80mW      | 120mW      | Backlight é o dominante           |
| WS2812 × 2 (30% brilho médio)       | 200mW     | 600mW      | 100% RGB = 360mA/LED = evitar     |
| INMP441                             | 5mW       | 7mW        | Negligível                        |
| MAX98357A (volume médio)            | 200mW     | 3.2W       | Pico @ 3W/4Ω com sinal máximo     |
| SCS0009 × 2 (movimento suave)       | 1.0W      | 8.0W       | Pico de stall ≈ 2A/servo × 2 × 5V |
| FE-TTLinker + alimentação 6V         | 100mW     | 200mW      | Rail servo separado do rail lógico |
| **Total típico**                    | **~1.9W** | **~12.7W** | Margem: ~2.3W (fonte = 15W)       |

A fonte aguenta o pico. O risco não é de sobrecarga da fonte — é de **queda de tensão transitória** no barramento 5V durante pico de servo (inrush de corrente).

---

## Capacitor Bulk (Obrigatório)

**C1: 470µF / 10V eletrolítico** (ou dois de 220µF em paralelo)
**C2: 100nF cerâmico** (desacoplamento de alta frequência)

Posicionamento: fisicamente o mais próximo possível dos conectores de alimentação dos servos. Fio de GND curto e grosso.

**Sem este capacitor:**

- Inrush de corrente ao iniciar movimento de servo causa queda de tensão de 200–400mV no rail 5V
- Queda se propaga para o LDO da placa
- Tensão 3.3V cai para ~3.0V → trigger de brownout → reset inesperado

**Verificação em bancada:**

- Com osciloscópio ou analisador de tensão barato: medir tensão no rail 5V durante movimento de servo
- Queda aceitável: <100mV
- Queda > 200mV: aumentar capacitância

---

## Brownout e Boot Safety

### Detector de Brownout (ESP32-S3)

O ESP32-S3 tem detector de brownout de hardware que monitora a tensão do LDO de 3.3V.

| Configuração               | Threshold aproximado |
| -------------------------- | -------------------- |
| Nível 0 (padrão)           | ~2.44V no rail 3.3V  |
| Nível 7 (mais conservador) | ~2.97V no rail 3.3V  |

**Configuração atual:** Nível 0. Ajustar empiricamente se houver resets espúrios durante testes em protoboard.

### Callback de Brownout

Ao detectar queda de tensão:

1. `power_monitor` recebe callback de hardware
2. Publica `NB_EVT_POWER_BROWNOUT_WARN` no event bus (fila de safety, alta prioridade)
3. `motion_safety` recebe o evento e **desabilita torque de todos os servos imediatamente**
4. `logger` registra evento com timestamp
5. Sistema aguarda recuperação de tensão ou reset de hardware

### Boot Safety e Sequência

**Problema:** Se o sistema reseta repetidamente por brownout com servo ativo, o servo pode se mover para posição aleatória a cada reset. Isso pode danificar a mecânica.

**Solução:**

- Ao detectar reset por brownout (`esp_reset_reason() == ESP_RST_BROWNOUT`): incrementa contador em NVS
- Se contador ≥ 3 resets por brownout consecutivos: entra em safe mode no próximo boot
- Em safe mode: servos não são ativados, independentemente de outros fatores

**Sequência de boot com foco em energia:**

```
1. UART + NVS init (sem carga periférica)
2. Verificar reset reason → logar
3. Verificar safe mode flag → ativar se necessário
4. Registrar brownout callback (antes de qualquer periférico)
5. Montar SD (opcional, falha não é fatal)
6. Init display (carga: ~80mW)
7. Init LEDs em baixo brilho (carga: ~50mW)
8. Motion safety init (sem torque ainda)
9. Servo PING (apenas leitura, sem movimento)
10. Boot sequence completa → ARMED (servos com torque habilitado AQUI)
```

Nenhum pico de corrente de servo ocorre durante o boot.

---

## Modos de Operação (Energia)

| Modo                      | Descrição           | Motion | Display | Audio     | Condição de Entrada |
| ------------------------- | ------------------- | ------ | ------- | --------- | ------------------- |
| `NB_POWER_NORMAL`         | Operação completa   | ✅     | ✅      | ✅        | Boot de sucesso     |
| `NB_POWER_SD_DEGRADED`    | SD ausente          | ✅     | ✅      | ❌ assets | SD não montou       |
| `NB_POWER_SAFE_MODE`      | Motion desabilitado | ❌     | ✅      | ✅        | 3 boots com falha   |
| `NB_POWER_EMERGENCY_STOP` | Só logging          | ❌     | ❌      | ❌        | Brownout ou comando |

---

## Considerações para Futura Versão com Bateria

Quando o sistema de bateria (LiPo 1S + bq25185 + MAX17048 + TPS61088) for adicionado:

**Sem impacto na fase inicial (decisões corretas desde já):**

- `power_monitor` já abstrai a fonte de energia → adicionar leitura de fuel gauge sem mudar interface
- Modos de baixo consumo (SLEEPING) já implementados → reusados para battery saving
- Eventos `NB_EVT_POWER_*` já no event bus → adicionar eventos de bateria sem refatorar

**O que NÃO hardcodar na fase inicial:**

- Nunca assumir "5V estável" em thresholds numéricos — usar NVS config
- Nunca assumir "alimentação infinita" em decisões de display brightness ou LED
- Modos de economia já implementados como comportamento (não como hacks de hardware)

**Impacto futuro do TPS61088 (boost 5V):**

- Boost adiciona tensão estável mesmo com bateria descarregada
- Os servos continuam recebendo 5V mesmo com célula em 3.0V
- A arquitetura atual é compatível — nenhuma mudança estrutural necessária
