# NodeBot — Contexto para Claude Code

## O que e este projeto

NodeBot e um companion robot desktop com ESP32-S3 como MCU principal. O robo tem expressividade visual, personalidade, interacao por voz, sensores, movimentos fisicos e operacao offline-first.

Este repositorio contem o firmware embarcado. Nao e um app — e firmware real com eletrica, energia, perifericos fisicos e risco mecanico.

## Stack

- MCU: ESP32-S3 (Freenove ESP32-S3-WROOM CAM N16R8)
- Framework: **ESP-IDF** (nao Arduino)
- RTOS: FreeRTOS (embutido no ESP-IDF)
- Linguagem: **C** (padrao C17)
- Build: CMake via `idf.py`

## Hardware congelado

| Componente        | Interface | Observacao                                  |
|-------------------|-----------|---------------------------------------------|
| OV2640            | DVP       | Camera onboard, trava ~12 pinos de GPIO     |
| ST7789 2"         | SPI       | Display, framebuffer em PSRAM               |
| 2x WS2812         | RMT       | LEDs RGB, timing critico                    |
| INMP441           | I2S0      | Microfone                                   |
| MAX98357A         | I2S1      | Amplificador/speaker                        |
| FE-TTLinker       | UART      | Conversor de sinal para servos SCS0009      |
| 2x SCS0009        | (via FE)  | Servos de bus serial Feetech, 5V            |
| Touch (cobre)     | Touch     | Periférico interno ESP32-S3                 |
| MPU-6050          | I2C       | IMU (acelerometro + giroscopio)             |
| MAX17048          | I2C       | Fuel gauge LiPo                             |
| bq25185           | I2C       | Carregador USB/DC/Solar LiPo                |
| TPS61088          | -         | Boost converter 5V 10A para servos          |
| LiPo 1S 3000mAh   | -         | Bateria principal                           |

## Arquitetura em camadas

```
Application  ->  Behavior, FSM de persona, interacao
Services     ->  Audio, Motion, Vision, Touch, IMU, Display, LED
Event Bus    ->  Desacoplamento entre servicos (FreeRTOS Queue)
Infra Core   ->  PowerManager, ConfigManager, Logger, Watchdog, BootManager
HAL/Drivers  ->  Um driver por periferico, sem logica de negocio
BSP/RTOS     ->  ESP-IDF, FreeRTOS, memory pools
```

## Convencoes

- Nenhum modulo chama API interna de outro servico diretamente — apenas via Event Bus ou API publica
- Nenhuma escrita direta em NVS — sempre via ConfigManager
- Nenhum driver contem logica de negocio
- Toda task registra heartbeat no WatchdogService
- Politica de erro documentada por modulo: HALT / DEGRADED / RETRY
- Alocacoes de DMA buffer: SRAM interna. Alocacoes grandes (framebuffer, ring buffer de audio): PSRAM
- Stack overflow checking habilitado em desenvolvimento (configCHECK_FOR_STACK_OVERFLOW=2)

## Estrutura do repositorio

```
noisebot/
  main/           # Ponto de entrada do ESP-IDF
  components/
    infra/        # boot_manager, logger, config_manager, watchdog, event_bus
    hal/          # drivers: display, led, servo, audio, imu, camera, touch, power
    services/     # display_service, led_service, motion_service, audio_service, ...
    app/          # behavior_fsm, persona
  docs/           # Documentacao tecnica do projeto
```

## Principios inegociaveis

1. Servos nao se movem sem safety layer ativa e validada
2. Sistema de energia e monitorado continuamente — LOW e CRITICAL tem acoes automaticas
3. Nenhum servico avancado inicializa com bateria em estado CRITICAL
4. Brownout reason e sempre logado e persistido em RTC memory
5. Safe mode ativa apos 3 boots com falha consecutiva

## Documentacao tecnica

- `docs/PROJECT.md` — visao geral
- `docs/HARDWARE.md` — hardware completo e pinagem
- `docs/ARCHITECTURE.md` — arquitetura detalhada
- `docs/ROADMAP.md` — roadmap e etapas executaveis
- `docs/ENERGY.md` — energia, brownout e boot safety
- `docs/SERVO_SAFETY.md` — motion safety
- `docs/RISKS.md` — riscos criticos
- `docs/INTEGRATION_STRATEGY.md` — ordem de integracao dos subsistemas
