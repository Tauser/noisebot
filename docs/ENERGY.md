# NodeBot — Energia, Brownout e Boot Safety

## Por que energia deve ser pensada desde o primeiro dia

Um sistema de energia mal entendido causa classes inteiras de bugs que parecem ser de software:

| Sintoma aparente                        | Causa real                                      |
|-----------------------------------------|-------------------------------------------------|
| Reset espontaneo que parece watchdog    | Brownout por queda de tensao sob carga          |
| I2C erratico que parece driver ruim     | Ripple do boost afetando o barramento           |
| NVS corrompida que parece bug de config | Reset no meio de write por undervoltage         |
| Servo "fugindo" de posicao              | Reinicializacao parcial por queda de tensao     |
| Comportamento erratico apos horas       | Bateria abaixo do threshold sem deteccao        |

Nenhum desses bugs pode ser diagnosticado sem observabilidade de energia construida desde cedo.

---

## Arquitetura do Sistema de Energia

### Power Path

```
USB / DC / Solar
       |
       v
  [bq25185 Charger]
  - Controla carregamento da LiPo
  - Status via I2C: BULK, ABSORPTION, FLOAT, FAULT
  - Protecoes: OVP, OCP, NTC (temperatura da celula)
       |
       v
  [LiPo 1S 3000mAh]
  - Nominal: 3.7V | Max: 4.2V | Cutoff sistema: 3.1V | Dano: < 2.5V
  - Monitorada por MAX17048 via I2C
       |
       |---> [TPS61088 Boost 5V/10A]
       |     - Alimenta servos SCS0009 (4.5-6V necessario)
       |     - Potencialmente LEDs WS2812 e MAX98357A
       |     - Gera ripple sob carga variavel — medir com osciloscópio
       |
       v
  [LDO 3.3V onboard Freenove]
       |
       v
  ESP32-S3, ST7789, INMP441, MPU-6050, MAX17048, bq25185
```

---

## Estados de Energia

```c
typedef enum {
    POWER_STATE_CHARGING,       // Carregando via USB/DC/Solar
    POWER_STATE_DISCHARGING,    // Descarregando normalmente
    POWER_STATE_LOW,            // SoC < LOW_THRESHOLD (default 20%)
    POWER_STATE_CRITICAL,       // SoC < CRITICAL_THRESHOLD (default 10%)
    POWER_STATE_FAULT,          // Fault detectado no charger
    POWER_STATE_UNKNOWN,        // Fuel gauge indisponivel
} nb_power_state_t;
```

### Thresholds (configurados via ConfigManager, persistidos em NVS)

| Threshold         | Default | Chave NVS              |
|-------------------|---------|------------------------|
| LOW_PCT           | 20%     | nb_power/low_pct       |
| CRITICAL_PCT      | 10%     | nb_power/critical_pct  |
| SHUTDOWN_MV       | 3100mV  | nb_power/shutdown_mv   |
| OVERTEMP_CHARGE_C | 45      | nb_power/otc_degc      |

### Acoes por estado

| Estado    | Acao imediata                                                          |
|-----------|------------------------------------------------------------------------|
| LOW       | Publicar EVT_POWER_LOW, LED amarelo, reducao de comportamento ativo    |
| CRITICAL  | Publicar EVT_POWER_CRITICAL, desligar torque dos servos, reduzir display, avisar |
| < 3.1V    | Shutdown sequencial: servos → camera → audio → display → safe halt     |
| FAULT     | Publicar EVT_POWER_FAULT, logar detalhes do fault bq25185, LED vermelho |

---

## Brownout

### Configuracao

O ESP32-S3 tem brownout detector de hardware configuravel via `menuconfig`:

```
CONFIG_ESP_BROWNOUT_DET=y
CONFIG_ESP_BROWNOUT_DET_LVL=7   # ~2.97V — recomendado
```

Nivel 7 (~2.97V) e recomendado para LiPo 1S pois:
- Mantem margem acima do ponto de dano (2.5V)
- Permite ao sistema reagir antes de comportamento erratico de perifericos
- 3.1V do LDO onboard pode cair abaixo do minimo se Vbat cair muito

### Handler customizado de brownout

```c
// Em boot_manager.c
static void brownout_handler(void) {
    // Executado antes do reset — tempo muito limitado
    // NAO chamar funcoes que alocam memoria ou usam mutexes

    // 1. Marcar flag em RTC memory (persiste apos reset)
    rtc_state.brownout_occurred = true;
    rtc_state.last_reset = ESP_RST_BROWNOUT;

    // 2. Log direto via UART (sem mutex, sem buffer)
    uart_write_bytes(UART_NUM_0, "BROWNOUT DETECTED\n", 18);
}

void boot_manager_init_brownout(void) {
    esp_brownout_init();  // Registra handler interno do IDF
    // Handler customizado registrado via esp_register_shutdown_handler()
}
```

### Recuperacao apos brownout

No proximo boot, verificar RTC memory:
1. Se `brownout_occurred == true`: logar aviso, checar SoC antes de continuar
2. Se SoC < CRITICAL_PCT: entrar em modo de emergencia (display apenas) ate carregar
3. Incrementar crash_count (brownout conta como crash para safe mode logic)
4. Limpar flag `brownout_occurred`

---

## Boot Safety

### RTC Memory (persiste apos brownout e WDT reset)

```c
// Em RTC_DATA_ATTR (persiste em RTC slow memory)
typedef struct {
    uint32_t            magic;              // 0xNB_BOOT para validacao
    uint32_t            boot_count;         // Total de boots
    uint32_t            crash_count;        // Boots sem clean shutdown
    esp_reset_reason_t  last_reset;         // Motivo do ultimo reset
    bool                brownout_occurred;  // Flag de brownout
    bool                safe_mode_active;   // Safe mode solicitado
    uint32_t            last_stable_ms;     // Timestamp do ultimo boot estavel
    uint8_t             _pad[2];
} nb_rtc_state_t;

static RTC_DATA_ATTR nb_rtc_state_t rtc_state;
```

### Logica de boot safety

```
boot_manager_init():
  1. Validar magic da RTC memory (primeiro boot = inicializar struct)
  2. Ler last_reset_reason via esp_reset_reason()
  3. Se reset reason != POWER_ON e != SW:
       crash_count++
  4. boot_count++
  5. Se crash_count >= 3:
       safe_mode_active = true
  6. Logar: boot_count, crash_count, last_reset, safe_mode

boot_manager_mark_stable():  // Chamado apos 5 min de operacao normal
  1. crash_count = 0
  2. last_stable_ms = esp_timer_get_time() / 1000
```

### Safe Mode

Quando `safe_mode_active == true`:

1. Inicializa apenas: Logger, PowerManager, Display, LEDs
2. **NAO inicializa:** Servos, Camera, Audio, Aplicacao
3. Display mostra: "SAFE MODE — Sistema instavel. Conecte via serial."
4. LED: roxo piscante lento
5. Aguarda intervencao via UART (comando `reset_safe_mode`)
6. Ao receber comando: zera crash_count, reinicia sistema

---

## Validacao do Power Path — Pre-Requisitos

### O que medir antes de ligar perifericos pesados

**Fase 1 — Sistema base (ESP32 + display + LEDs):**
- [ ] Tensao 3.3V estavel sob carga (medir com multimetro e osciloscópio)
- [ ] Corrente total do sistema base (medir com amperimetro em serie)
- [ ] Ripple no 3.3V (deve ser < 20mV)

**Fase 2 — Boost converter (TPS61088):**
- [ ] Tensao de saida sem carga: 5.0V ± 5%
- [ ] Tensao de saida com carga resistiva simulada (1A, 2A, 3A)
- [ ] Ripple em 5V sob transiente de carga (deve ser < 50mV)
- [ ] Verificar que ripple do 5V nao aparece no 3.3V (isolacao adequada)

**Fase 3 — Servos em movimento:**
- [ ] Medir corrente de pico de 1 servo em aceleracao
- [ ] Medir corrente de pico de 2 servos em aceleracao simultanea
- [ ] Verificar que Vbat nao cai abaixo de 3.3V durante pico (margem de seguranca)
- [ ] Verificar que brownout nao dispara durante movimento normal

**Fase 4 — Sistema completo:**
- [ ] Todos os perifericos ativos simultaneamente: medir corrente total
- [ ] Verificar autonomia estimada: C_bateria(mAh) / I_medio(mA) = horas

### Criterios de aprovacao da validacao

| Medida                  | Criterio de aprovacao          |
|-------------------------|--------------------------------|
| Vout boost sem carga    | 5.0V ± 0.25V                  |
| Ripple boost sob 3A     | < 50mV pico a pico             |
| Vbat minima durante pico| > 3.3V (acima do brownout)     |
| Corrente sistema base   | < 500mA @ 3.7V                |
| Corrente 2 servos mov.  | Medida documentada             |

Se qualquer criterio falhar: resolver problema eletrico antes de prosseguir para Bloco 3.

---

## Perfis de Consumo (estimativas — substituir por medicoes reais)

| Configuracao                    | Corrente estimada @ 3.7V |
|---------------------------------|--------------------------|
| ESP32-S3 idle + WiFi off        | ~80mA                    |
| + Display ST7789                | +30mA                    |
| + LEDs WS2812 (branco max)      | +120mA (60mA por LED)    |
| + Audio playback (1W)           | +200mA (via boost)       |
| + 1 servo em movimento          | +400-800mA (via boost)   |
| + 2 servos em movimento         | +800-1500mA (via boost)  |
| + Camera OV2640 ativa           | +100-200mA               |
| **Total maximo estimado**       | **~2.5-3A @ 3.7V**       |

Com bateria de 3000mAh: autonomia estimada de ~1-1.5h em operacao plena.

> Estes valores sao estimativas. Substituir por medicoes reais na Etapa 2.1.
