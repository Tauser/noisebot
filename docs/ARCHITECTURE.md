# NoiseBot — Arquitetura

## Camadas do Sistema

```
┌─────────────────────────────────────────────────────────┐
│  LAYER 8: EXPANSÕES FUTURAS                             │
│  camera_hal, imu_hal, battery_service                   │
├─────────────────────────────────────────────────────────┤
│  LAYER 7: PERSONA E MEMÓRIA LONGA                       │
│  persona_service, long_term_memory                      │
├─────────────────────────────────────────────────────────┤
│  LAYER 6: COMPORTAMENTO E ESTADO INTERNO                │
│  behavior_engine, state_machine, emotion_model          │
├─────────────────────────────────────────────────────────┤
│  LAYER 5: SERVIÇOS CENTRAIS                             │
│  gaze_service, idle_service, expression_service,        │
│  conductor                                              │
├─────────────────────────────────────────────────────────┤
│  LAYER 4: DOMÍNIOS DE OUTPUT/INPUT                      │
│  render_service, motion_service, audio_service,         │
│  led_service, touch_service                             │
├─────────────────────────────────────────────────────────┤
│  LAYER 3: SAFETY E PROTEÇÃO                             │
│  motion_safety, power_monitor, error_policy             │
├─────────────────────────────────────────────────────────┤
│  LAYER 2: INFRAESTRUTURA                                │
│  event_bus, logger, config_manager, persistence_mgr,    │
│  watchdog_service, boot_manager, error_policy           │
├─────────────────────────────────────────────────────────┤
│  LAYER 1: HAL (Hardware Abstraction)                    │
│  display_hal, servo_hal, audio_hal, led_hal,            │
│  touch_hal, sd_hal                                      │
├─────────────────────────────────────────────────────────┤
│  LAYER 0: FUNDAÇÃO                                      │
│  ESP-IDF drivers, FreeRTOS, partitions, sdkconfig       │
└─────────────────────────────────────────────────────────┘
```

**Regra de chamada:** Layer N só chama Layer N-1 ou inferior.
Comunicação entre layers não adjacentes é sempre via event bus.

---

## Estrutura de Componentes ESP-IDF

```
components/
├── infra/
│   ├── CMakeLists.txt
│   ├── boot_manager.c / .h      # Sequência de boot com fases
│   ├── logger.c / .h            # Logging com timestamp, nível, módulo
│   ├── event_bus.c / .h         # Pub/sub com pool estático
│   ├── nb_events.h              # Tipos de evento (enum + payload union)
│   ├── config_manager.c / .h    # Abstração sobre NVS
│   ├── nb_config_keys.h         # Todas as chaves de configuração
│   ├── persistence_mgr.c / .h   # Abstração sobre NVS + SD
│   ├── watchdog_service.c / .h  # TWDT e HW WDT
│   ├── error_policy.h           # Macros de assert e política de erro
│   └── nb_persist_types.h       # Tipos das estruturas persistidas
│
├── hal/
│   ├── CMakeLists.txt
│   ├── display_hal.cpp / .h     # LovyanGFX + wrapper C (extern "C")
│   ├── display_lgfx_config.hpp  # Configuração do panel ST7789
│   ├── servo_hal.c / .h         # UART + protocolo SCSCL Feetech
│   ├── audio_hal.c / .h         # I2S0 (mic) + I2S1 (speaker)
│   ├── led_hal.c / .h           # RMT + WS2812
│   ├── touch_hal.c / .h         # Touch peripheral ESP32-S3
│   ├── sd_hal.c / .h            # SPI3 + FATFS + mount
│   └── nb_hw_config.h           # GPIO, limites HW, constantes de hardware
│
├── safety/
│   ├── CMakeLists.txt
│   ├── motion_safety.c / .h     # Limites, stall detection, heartbeat, fail-safe
│   └── power_monitor.c / .h     # Brownout callback, modos de energia
│
├── services/
│   ├── CMakeLists.txt
│   ├── render_service.c / .h    # Render loop, layer system, FPS control
│   ├── expression_primitives.c/.h # Primitivos de face procedural
│   ├── motion_service.c / .h    # Interpolação, primitivos de pescoço
│   ├── audio_service.c / .h     # Playback WAV do SD
│   ├── led_service.c / .h       # Animações de LED
│   ├── touch_service.c / .h     # Detecção TAP/LONG/SUSTAINED + eventos
│   ├── gaze_service.c / .h      # Saccade model, gaze targets
│   ├── idle_service.c / .h      # Microbehaviors de idle
│   ├── expression_service.c/.h  # Interpolação de face_state, fila de expressões
│   └── conductor.c / .h         # Coordenação face/motion/áudio
│
├── behavior/
│   ├── CMakeLists.txt
│   ├── behavior_engine.c / .h   # Roteamento de eventos → ações
│   ├── state_machine.c / .h     # Estados de alto nível do robot
│   └── emotion_model.c / .h     # Vetor (valência, ativação), decaimento
│
└── persona/
    ├── CMakeLists.txt
    ├── persona_service.c / .h   # Persona seed, preferências, modulação
    └── long_term_memory.c / .h  # interaction_history, event_journal, stats
```

---

## Event Bus

### Contrato

Pool estático de `NB_EVENT_POOL_SIZE` eventos (sem malloc por evento).
Dois modos de entrega:

- **Síncrono:** subscriber chamado na task do publisher. Zero latência. Subscriber não pode bloquear.
- **Assíncrono:** evento entra na fila FreeRTOS da task destino.

Fila separada para eventos de safety — nunca bloqueada por backpressure normal.

### Tipos de Evento (nb_events.h)

```c
typedef enum {
    /* Touch */
    NB_EVT_TOUCH_TAP,
    NB_EVT_TOUCH_LONG_PRESS,
    NB_EVT_TOUCH_SUSTAINED,
    NB_EVT_TOUCH_BEGIN,
    NB_EVT_TOUCH_END,
    NB_EVT_TOUCH_WAKE,

    /* Áudio */
    NB_EVT_AUDIO_STARTED,       /* payload: duration_ms */
    NB_EVT_AUDIO_ENDED,
    NB_EVT_VOICE_ACTIVITY_START,
    NB_EVT_VOICE_ACTIVITY_END,

    /* Motion / Safety */
    NB_EVT_SERVO_STALL_DETECTED,   /* payload: servo_id */
    NB_EVT_SERVO_LIMIT_HIT,        /* payload: servo_id, direction */
    NB_EVT_SERVO_TEMP_WARN,        /* payload: servo_id, temp_c */
    NB_EVT_MOTION_HEARTBEAT_LOST,
    NB_EVT_MOTION_ARMED,
    NB_EVT_MOTION_DISARMED,

    /* Energia */
    NB_EVT_POWER_BROWNOUT_WARN,
    NB_EVT_POWER_MODE_CHANGED,     /* payload: nb_power_mode_t */

    /* Comportamento */
    NB_EVT_BEHAVIOR_STATE_CHANGED, /* payload: nb_behavior_state_t */
    NB_EVT_EMOTION_CHANGED,        /* payload: nb_emotion_t */
    NB_EVT_GAZE_TARGET_SET,        /* payload: pan, tilt */
    NB_EVT_IDLE_TRIGGERED,
    NB_EVT_ACTION_REQUESTED,       /* payload: nb_action_t */

    /* Sistema */
    NB_EVT_BOOT_COMPLETE,
    NB_EVT_SD_MOUNTED,
    NB_EVT_SD_ERROR,
    NB_EVT_SD_DEGRADED,

    NB_EVT_COUNT
} nb_event_type_t;
```

### API

```c
esp_err_t event_bus_init(void);
esp_err_t event_bus_publish(nb_event_type_t type,
                             const nb_event_payload_t *payload);
esp_err_t event_bus_subscribe(nb_event_type_t type,
                               nb_event_handler_t handler,
                               void *ctx);
esp_err_t event_bus_unsubscribe(nb_event_type_t type,
                                 nb_event_handler_t handler);
```

---

## Tasks FreeRTOS

| Task                   | Componente       | Core | Prioridade | Stack | Notas                  |
| ---------------------- | ---------------- | ---- | ---------- | ----- | ---------------------- |
| `nb_wdog_task`         | watchdog_service | 0    | 24         | 2KB   | TWDT reporter          |
| `nb_servo_safety_task` | motion_safety    | 1    | 23         | 4KB   | Poll load/temp 20Hz    |
| `nb_motion_task`       | motion_service   | 1    | 20         | 4KB   | Interpolação posição   |
| `nb_audio_task`        | audio_service    | 0    | 18         | 8KB   | I2S DMA feeding        |
| `nb_render_task`       | render_service   | 1    | 15         | 8KB   | SPI display 30-60fps   |
| `nb_behavior_task`     | behavior_engine  | 1    | 12         | 6KB   | Estado + ações         |
| `nb_touch_task`        | touch_service    | 0    | 10         | 3KB   | Poll touch 50Hz        |
| `nb_led_task`          | led_service      | 0    | 8          | 2KB   | RMT updates            |
| `nb_persist_task`      | persistence_mgr  | 0    | 5          | 4KB   | SD writes não-urgentes |
| `nb_logger_task`       | logger           | 0    | 3          | 4KB   | Flush log para SD      |

**Regra:** Tasks de safety (prioridade ≥ 20) nunca preemptadas por tasks de comportamento (prioridade ≤ 15).

---

## LovyanGFX — Contrato Arquitetural C++/C

### Problema

LovyanGFX é C++. O projeto é C17. A solução é um componente wrapper:

```
hal/display_lgfx_config.hpp   →  Configuração do panel (C++ puro)
hal/display_hal.cpp            →  Instância LGFX, operações, mutex (C++)
hal/display_hal.h              →  API pública com extern "C" (visível em C)
```

Nenhum código C inclui headers C++ do LovyanGFX diretamente.
Toda interação com o display passa por `display_hal.h`.

### Configuração do Panel

```cpp
// display_lgfx_config.hpp
class LGFX_NoiseBot : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_SPI      _bus;
    lgfx::Light_PWM    _light;
public:
    LGFX_NoiseBot(void) {
        { auto cfg = _bus.config();
          cfg.spi_host    = SPI2_HOST;
          cfg.freq_write  = 40000000;    // 40MHz, testar 80MHz
          cfg.freq_read   = 16000000;
          cfg.use_lock    = true;        // thread-safe interno
          cfg.dma_channel = SPI_DMA_CH_AUTO;
          cfg.pin_sclk    = NB_PIN_DISP_SCLK;
          cfg.pin_mosi    = NB_PIN_DISP_MOSI;
          cfg.pin_miso    = NB_PIN_DISP_MISO;
          cfg.pin_dc      = NB_PIN_DISP_DC;
          _bus.config(cfg); _panel.setBus(&_bus); }
        { auto cfg = _panel.config();
          cfg.pin_cs        = NB_PIN_DISP_CS;
          cfg.pin_rst       = NB_PIN_DISP_RST;
          cfg.panel_width   = 240;
          cfg.panel_height  = 240;
          _panel.config(cfg); }
        { auto cfg = _light.config();
          cfg.pin_bl      = NB_PIN_DISP_BL;
          cfg.pwm_channel = 0;
          cfg.freq        = 44100;
          _light.config(cfg); _panel.setLight(&_light); }
        setPanel(&_panel);
    }
};
```

Todos os pinos `NB_PIN_*` definidos em `hal/nb_hw_config.h`.

### Framebuffer e Sprites

- Sprite principal (face canvas): 240×240 @ 16bpp = ~115KB em PSRAM
- `LGFX_Sprite` com `setPsram(true)`
- Double buffering: dois sprites A/B em PSRAM; render em B enquanto A é enviado via SPI DMA
- Verificar empiricamente no bring-up se DMA SPI alcança PSRAM no S3

### Thread Safety

- Mutex interno de `display_hal.cpp` protege todos os acessos ao objeto LGFX
- Nenhum código fora de `display_hal.cpp` toca o objeto LGFX diretamente
- Sprites são alocados/liberados apenas pela `render_task`

---

## Política de Memória

| Recurso      | Regra                                                                                                          |
| ------------ | -------------------------------------------------------------------------------------------------------------- |
| SRAM (512KB) | FreeRTOS kernel, stacks de tasks, I2S DMA buffers (DMA não alcança PSRAM via I2S), variáveis de estado crítico |
| PSRAM (8MB)  | Framebuffers de display, buffers de áudio secundários, circular buffer de LTM, futuro frame buffer de câmera   |
| Flash/NVS    | Config crítica, flags de safety, calibração, persona seed                                                      |
| microSD      | Logs, assets de áudio, memória de longo prazo                                                                  |

**Headroom obrigatório em PSRAM:** manter ≥300KB livres para futuro frame buffer da câmera.
Monitorar em produção: `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)`.

---

## Pipeline de Render

```
behavior_engine
      │ (emotion_t)
      ▼
emotion_model ──► face_state_t target
      │
      ▼
expression_service ──► current_face_state_t (interpolada a cada frame)
      │
      ▼
render_service ──► render_layer_fn_t callbacks
      │            [face_layer, overlay_layer, debug_layer]
      ▼
LGFX_Sprite (PSRAM, 240×240)
      │
      ▼
display_hal_sprite_push() ──► LovyanGFX SPI DMA ──► ST7789
```

---

## Boot Sequence

```
app_main()
    │
    └── boot_manager_run()
            │
            ├── PHASE_EARLY      UART, HW WDT, NVS init, reset reason
            ├── PHASE_POWER      brownout callback, power_monitor init
            ├── PHASE_STORAGE    microSD mount, persistence_mgr init
            ├── PHASE_HAL        display, LEDs, touch, servo PING (sem movimento)
            ├── PHASE_SAFETY     motion_safety init, safety checks
            ├── PHASE_SERVICES   render, audio, behavior, gaze, conductor
            ├── PHASE_MOTION     servos ARMED (só após safety confirmado)
            └── PHASE_COMPLETE   boot_count reset, idle behavior start
```

Falha em fase crítica (EARLY, POWER, SAFETY) → safe mode (motion desabilitado).
Falha em fase não-crítica (STORAGE) → modo degradado (SD absent).
