# NodeBot — Arquitetura do Sistema

## Principios Arquiteturais

1. **Nenhum servico chama API interna de outro diretamente** — apenas via Event Bus ou API publica
2. **Nenhuma escrita direta em NVS** — sempre via ConfigManager
3. **Nenhum driver contem logica de negocio** — drivers sao abstrações de hardware puro
4. **Toda task registra heartbeat no WatchdogService**
5. **Politica de erro documentada por modulo** antes de qualquer integracao
6. **DMA buffers em SRAM interna** — nunca em PSRAM (latencia incompativel com DMA)
7. **Buffers grandes (framebuffer, audio ring) em PSRAM**
8. **microSD nao e periferico opcional** — e camada de persistencia central do sistema
9. **Escrita no SD sempre via PersistenceManager** — nunca diretamente do servico de comportamento
10. **Boot critico (fases 0-2) tem zero dependencia de microSD** — SD falho = modo amnesico, nao HALT

> Estrategia completa de persistencia e memoria de longo prazo: ver `docs/PERSISTENCE.md`

---

## Modelo de Camadas

```
┌─────────────────────────────────────────────────────────────┐
│                      APLICACAO                              │
│    Persona · Comportamento · FSM de interacao               │
│    app/behavior_fsm.c · app/persona.c                       │
├─────────────────────────────────────────────────────────────┤
│                SERVICOS DE ALTO NIVEL                       │
│    AudioService · VisionService · BehaviorService           │
├─────────────────────────────────────────────────────────────┤
│                SERVICOS INTERMEDIARIOS                      │
│    MotionService · IMUService · TouchService                │
│    DisplayService · LEDService · StorageService             │
├─────────────────────────────────────────────────────────────┤
│                     EVENT BUS                               │
│    Desacoplamento entre servicos via eventos tipados        │
│    infra/event_bus.h — FreeRTOS Queue + tipagem de eventos  │
├─────────────────────────────────────────────────────────────┤
│                 INFRAESTRUTURA CORE                         │
│    PowerManager · ConfigManager · Logger                    │
│    WatchdogService · BootManager · ErrorPolicy              │
│    infra/                                                   │
├─────────────────────────────────────────────────────────────┤
│                  HAL / DRIVERS                              │
│    display_drv · servo_drv · audio_drv · led_drv           │
│    imu_drv · fuel_gauge_drv · charger_drv                  │
│    camera_drv · touch_drv · storage_drv                    │
│    hal/                                                     │
├─────────────────────────────────────────────────────────────┤
│                 RTOS / BSP / SDK                            │
│    FreeRTOS · ESP-IDF · Memory pools · IPC primitives       │
└─────────────────────────────────────────────────────────────┘
```

---

## Estrutura de Diretorios

```
noisebot/
  main/
    main.c              # Ponto de entrada — chama boot_manager_start()
    CMakeLists.txt
  components/
    infra/              # Infraestrutura core
      boot_manager.h/c        # Fases de boot, reset reason, safe mode
      logger.h/c              # Logger estruturado com buffer circular
      config_manager.h/c      # API sobre NVS com schema versionado
      event_bus.h/c           # Event bus tipado via FreeRTOS Queue
      watchdog_service.h/c    # Heartbeat multicamada
      power_manager.h/c       # Estado global de energia, politicas
      persistence_manager.h/c # Persistencia em camadas: NVS + SD + memoria longa
      error_policy.h          # Definicoes de politica de erro por modulo
    hal/                # Drivers de hardware
      display/
        st7789_drv.h/c
      led/
        ws2812_drv.h/c
      servo/
        scservo_drv.h/c # Protocolo SCServo via FE-TTLinker
      audio/
        inmp441_drv.h/c
        max98357a_drv.h/c
      imu/
        mpu6050_drv.h/c
      power/
        max17048_drv.h/c # Fuel gauge
        bq25185_drv.h/c  # Carregador
      storage/
        sd_storage_drv.h/c
      camera/
        ov2640_drv.h/c
      touch/
        touch_drv.h/c
    services/           # Logica de servico sobre drivers
      display_service.h/c
      led_service.h/c
      motion_service.h/c  # Inclui safety layer de movimento
      audio_service.h/c
      imu_service.h/c
      touch_service.h/c
      storage_service.h/c
      camera_service.h/c
    app/                # Aplicacao e comportamento
      behavior_fsm.h/c
      persona.h/c
  docs/
  .gitignore
  CLAUDE.md
  CMakeLists.txt
  sdkconfig               # Configuracao do ESP-IDF (commitado)
```

---

## Event Bus

### Design

O event bus e a espinha dorsal de comunicacao entre servicos. Nenhum servico conhece o internals de outro — apenas publica e consome eventos tipados.

```c
// event_bus.h

typedef enum {
    // Sistema
    EVT_SYSTEM_BOOT_COMPLETE,
    EVT_SYSTEM_SAFE_MODE,
    EVT_SERVICE_STARTED,
    EVT_SERVICE_FAILED,
    EVT_ERROR_REPORTED,

    // Energia
    EVT_POWER_CHARGING,
    EVT_POWER_DISCHARGING,
    EVT_POWER_LOW,        // SoC < LOW_THRESHOLD
    EVT_POWER_CRITICAL,   // SoC < CRITICAL_THRESHOLD
    EVT_POWER_FAULT,      // Fault no charger

    // Movimento
    EVT_MOTION_COMMAND,
    EVT_MOTION_COMPLETE,
    EVT_MOTION_FAULT,
    EVT_SERVO_OVERTEMP,
    EVT_SERVO_STALL,

    // Sensores
    EVT_TOUCH_START,
    EVT_TOUCH_END,
    EVT_TOUCH_HOLD,
    EVT_IMU_TILT,
    EVT_IMU_FALL,

    // Audio
    EVT_AUDIO_CAPTURE_READY,
    EVT_AUDIO_PLAYBACK_START,
    EVT_AUDIO_PLAYBACK_COMPLETE,
    EVT_VOICE_DETECTED,

    // Comportamento
    EVT_BEHAVIOR_TRIGGER,
    EVT_BEHAVIOR_COMPLETE,
} nb_event_type_t;

typedef struct {
    nb_event_type_t type;
    uint32_t        timestamp_ms;
    const char*     source_module;
    union {
        struct { uint8_t soc_pct; uint16_t voltage_mv; } power;
        struct { int16_t pos; uint8_t servo_id; }        motion;
        struct { uint32_t duration_ms; }                  touch;
        struct { float ax, ay, az; float gx, gy, gz; }   imu;
        // ... outros payloads
    } data;
} nb_event_t;

esp_err_t event_bus_publish(const nb_event_t *evt);
esp_err_t event_bus_subscribe(nb_event_type_t type, QueueHandle_t queue);
```

### Regras do Event Bus

- `event_bus_publish()` nunca bloqueia indefinidamente — usa timeout de 10ms
- Chamada de publish em ISR context e proibida
- Subscribers recebem copia do evento — sem referencias a memoria compartilhada no payload
- Eventos sao processados em task dedicada (EventDispatchTask, Core 1)

---

## Modelo de Inicializacao por Fases

O sistema nao inicializa todos os perifericos de uma vez. Segue protocolo sequencial com verificacao de saude em cada transicao.

```
BOOT_PHASE_0: Watchdog, Logger, NVS, Reset Reason, Boot Flags
              → sem perifericos externos, sem I2C, sem SPI
              → se falhar: PANIC imediato (sem recovery possivel)

BOOT_PHASE_1: ConfigManager, EventBus
              → infraestrutura de comunicacao interna
              → se falhar: PANIC

BOOT_PHASE_2: PowerManager (MAX17048 + bq25185)
              → conhecer estado de energia ANTES de ligar qualquer coisa
              → se fuel gauge falhar: assumir 50% SoC, logar aviso
              → se charger falhar: logar aviso, continuar

BOOT_PHASE_3: Storage (microSD), Display, LEDs
              → feedback visual disponivel a partir daqui
              → se microSD falhar: DEGRADED (log apenas UART)
              → se display falhar: DEGRADED (continuar sem display)

BOOT_PHASE_4: IMU (MPU-6050), Touch
              → sensores passivos — sem risco
              → se falhar: DEGRADED (funcionar sem aquele sensor)

BOOT_PHASE_5: Servos (comunicacao e status apenas — sem movimento)
              → verificar condicao de energia antes de inicializar
              → se CRITICAL: adiar inicializacao de servo
              → se falhar comunicacao: DEGRADED (sem movimento)

BOOT_PHASE_6: Audio (INMP441 + MAX98357A)
              → se falhar: DEGRADED (sem audio)

BOOT_PHASE_7: Camera (OV2640)
              → verificar heap disponivel antes de inicializar
              → se heap < 40KB livre: nao inicializar camera, logar
              → se falhar: DEGRADED (sem visao)

BOOT_PHASE_8: Aplicacao (BehaviorFSM, Persona)
              → so inicia se servicos minimos criticos OK
```

---

## Politica de Erros por Modulo

Cada modulo declara sua politica de erro explicitamente:

| Modulo           | Falha no boot       | Falha em operacao         |
|------------------|---------------------|---------------------------|
| Logger           | PANIC               | Nao aplicavel             |
| WatchdogService  | PANIC               | Nao aplicavel             |
| ConfigManager    | PANIC               | LOG + usar default        |
| EventBus         | PANIC               | LOG + retry (3x) + HALT   |
| PowerManager     | DEGRADED (sem SoC)  | LOG + usar ultimo valor   |
| Display          | DEGRADED            | LOG + continuar sem tela  |
| LEDs             | DEGRADED            | LOG + continuar sem LED   |
| Storage (SD)     | DEGRADED            | LOG + modo UART-only      |
| IMU              | DEGRADED            | LOG + sem deteccao de tilt|
| Touch            | DEGRADED            | LOG + sem input tactil    |
| Servos           | DEGRADED            | LOG + modo sem movimento  |
| Audio            | DEGRADED            | LOG + modo silencioso     |
| Camera           | DEGRADED            | LOG + modo sem visao      |

---

## ConfigManager

### Design

Toda configuracao passa pelo ConfigManager. Nenhum modulo escreve direto em NVS.

```c
// Namespace por modulo
#define CFG_NS_POWER    "nb_power"
#define CFG_NS_SERVO    "nb_servo"
#define CFG_NS_AUDIO    "nb_audio"
#define CFG_NS_SYSTEM   "nb_system"

// API
esp_err_t config_init(void);
esp_err_t config_get_u8(const char *ns, const char *key, uint8_t *val, uint8_t default_val);
esp_err_t config_set_u8(const char *ns, const char *key, uint8_t val);
esp_err_t config_get_u16(const char *ns, const char *key, uint16_t *val, uint16_t default_val);
esp_err_t config_set_u16(const char *ns, const char *key, uint16_t val);
// ... outros tipos

uint8_t   config_get_schema_version(void);
esp_err_t config_migrate(uint8_t from_version, uint8_t to_version);
```

---

## WatchdogService

### Dois niveis de watchdog

**Nivel 1 — Hardware WDT (ESP-IDF Task WDT):**
- Configurado via `esp_task_wdt_init()` com timeout (ex: 10s)
- Tasks criticas registradas com `esp_task_wdt_add()`
- Task que nao resetar o WDT dentro do timeout = reset do sistema

**Nivel 2 — Software Heartbeat:**
- Cada servico registra no WatchdogService com intervalo esperado
- WatchdogService verifica periodicamente se todos os heartbeats chegaram
- Ausencia = log de alerta + publicacao de EVT_SERVICE_FAILED no event bus

```c
// watchdog_service.h
esp_err_t watchdog_register_service(const char *name, uint32_t expected_interval_ms);
esp_err_t watchdog_heartbeat(const char *name);
void      watchdog_monitor_task(void *arg); // task periodica de verificacao
```

---

## Boot Safety

### Boot Counter e Safe Mode

```c
// Em RTC memory (persiste atraves de brownout e watchdog reset)
typedef struct {
    uint32_t    boot_count;
    uint32_t    crash_count;        // resets sem CLEAN_BOOT flag
    esp_reset_reason_t last_reset;
    bool        brownout_occurred;
    bool        safe_mode_active;
    uint8_t     _padding[2];
} nb_rtc_state_t;
```

Logica de boot safety:
1. Ler `nb_rtc_state_t` da RTC memory
2. Se `crash_count >= 3`: ativar SAFE_MODE
3. Ao final de boot bem-sucedido (apos 5 min estavel): zerar `crash_count`
4. SAFE_MODE: inicia apenas Logger + PowerManager + Display — aguarda intervencao serial ou touch

---

## Gestao de Memoria

### Regras de alocacao

| Tipo de dado                        | Onde alocar  | Motivo                          |
|-------------------------------------|-------------|----------------------------------|
| DMA buffer (I2S, SPI, camera)       | SRAM interna | DMA nao pode acessar PSRAM      |
| Framebuffer display (150KB)         | PSRAM        | Muito grande para SRAM interna  |
| Audio ring buffer (capture)         | PSRAM        | Grande, acesso nao-critico       |
| Camera frame buffer (~200KB)        | PSRAM        | Muito grande para SRAM interna  |
| FreeRTOS stacks                     | SRAM interna | Acesso frequente e critico       |
| Configuracao em RAM                 | SRAM interna | Pequena, acesso frequente        |

### Alocacao em PSRAM

```c
// Usar sempre MALLOC_CAP_SPIRAM para PSRAM
void *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

// Verificar sucesso — falha de alocacao de PSRAM NAO e fatal para toda alocacao
if (!buf) {
    ESP_LOGE(TAG, "PSRAM alloc failed: %zu bytes", size);
    // tratar conforme politica do modulo
}
```

### Monitoramento de heap

- Task de monitor loga heap livre a cada 60s
- Alerta publicado no event bus se heap SRAM < 20KB
- Alerta se heap PSRAM < 100KB (camera em risco)

---

## Distribuicao de Tasks (FreeRTOS)

| Task                  | Core | Prioridade | Stack    | Periodicidade |
|-----------------------|------|------------|----------|---------------|
| EventDispatchTask     | 1    | 8          | 4KB      | event-driven  |
| PowerMonitorTask      | 1    | 7          | 2KB      | 1s            |
| WatchdogMonitorTask   | 1    | 9          | 2KB      | 1s            |
| ServoTask             | 1    | 6          | 4KB      | 10ms          |
| IMUTask               | 1    | 5          | 3KB      | 20ms (50Hz)   |
| TouchTask             | 1    | 4          | 2KB      | 20ms          |
| DisplayTask           | 1    | 3          | 4KB      | 33ms (30fps)  |
| AudioCaptureTask      | 1    | 6          | 4KB      | DMA-driven    |
| AudioPlaybackTask     | 1    | 5          | 4KB      | DMA-driven    |
| LoggerTask            | 1    | 2          | 4KB      | event-driven  |
| BehaviorTask          | 1    | 3          | 6KB      | event-driven  |
| HeapMonitorTask       | 1    | 1          | 2KB      | 60s           |

Notas:
- Core 0 reservado para WiFi/BT stack (se ativo)
- Prioridade 9 = maxima para tasks criticas de seguranca
- Stack overflow checking: `configCHECK_FOR_STACK_OVERFLOW=2` em desenvolvimento
