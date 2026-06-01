# AGENTS.md — NoiseBot Firmware

Instruções para o assistente de IA trabalhando neste repositório.
Este arquivo tem autoridade máxima sobre qualquer instrução geral.

---

## Workflow

**Não usar skills GSD** (`/gsd:*`) neste projeto. Trabalhar diretamente — planejamento e execução inline sem subagentes ou orchestrators GSD.

### Knowledge OS

- O Personal Knowledge OS do projeto fica em `D:\base_conhecimento\projects\Noisebot`.
- Ao fazer mudanças relevantes no projeto, atualizar também a base de conhecimento de forma ampla: `source/`, `wiki/`, `wiki/modules/`, `control/`, índices e logs afetados.
- Não atualizar apenas a página diretamente relacionada; sincronizar overview, arquitetura, mapa de código, setup/operação, testes, dependências, integrações, riscos, glossário, status, decisões e auditoria quando forem impactados.
- Toda atualização da base deve citar evidências do projeto original ou marcar claramente `Inferência da IA` / `Precisa de verificação`.
- A política detalhada fica em `D:\base_conhecimento\projects\Noisebot\control\update-policy.md`.

---

## Projeto

**NoiseBot** é um companion robot desktop expressivo baseado em ESP32-S3.
O firmware é C17 puro, salvo o componente `nb_hal/display` (C++ para LovyanGFX).
A stack é ESP-IDF + FreeRTOS. **Nunca usar Arduino.**

---

## Regras Inegociáveis

### Stack e Linguagem

- ESP-IDF (não Arduino). Toda API vem de `esp_*` ou `freertos/`.
- C17 em todos os componentes exceto `nb_hal/display` (C++ exclusivamente para LovyanGFX).
- CMake via `idf_component_register`. Não usar Makefile legado.
- Compilar com `-Wall -Wextra -Werror` — zero warnings tolerados.

### Arquitetura em Camadas

- Camadas só chamam para baixo (Layer N → Layer N-1 ou inferior).
- Comunicação entre camadas não adjacentes: **sempre via event bus**.
- Nenhum componente de comportamento (Layer 5-7) chama HAL diretamente.
- Nenhum HAL publica no event bus diretamente — passes para o serviço da Layer 4.

### Baseline de Comportamento

- `IDLE` é sempre o baseline persistente visual e comportamental do robô.
- A base de `IDLE` é expressão `NEUTRAL`, gaze central, pescoço central e LED idle.
- Expressões/ações como `CURIOUS`, `HAPPY`, `FOCUSED`, `ATTENTIVE`, touch, wake e fala são momentos transitórios ou overlays. Elas nunca substituem o baseline de `IDLE`.
- Toda entrada em `IDLE` deve limpar expressão, gaze, postura e overlays transitórios antes de aceitar novos comportamentos.
- `SLEEPING`, `MEDITATION`, `SILENT_COMPANY`, `RESPONDING` e estados de erro podem ter bases próprias, mas ao sair deles para `IDLE` o baseline de `IDLE` volta a ter autoridade.

```
Layer 0: ESP-IDF / FreeRTOS / Hardware
Layer 1: HAL        (nb_hal/display, nb_hal/servo, nb_hal/audio, nb_hal/led, nb_hal/touch, nb_hal/sd)
Layer 2: Infra      (event_bus, logger, config_manager, persistence_mgr, watchdog, boot_manager)
Layer 3: Safety     (motion_safety, power_monitor, error_policy)
Layer 4: Services   (render_service, motion_service, audio_service, led_service, touch_service)
Layer 5: Core Svcs  (gaze_service, idle_service, expression_service, conductor)
Layer 6: Behavior   (behavior_engine, state_machine, emotion_model)
Layer 7: Persona    (persona_service, long_term_memory)
Layer 8: Futuro     (camera, imu, battery)
```

### Motion Safety — Regra de Veto

- **Nenhum movimento de servo é implementado antes de `motion_safety` estar verde.**
- `motion_safety` tem autoridade de veto sobre qualquer comando de posição.
- Toda escrita de posição passa obrigatoriamente por `motion_safety_check_position()`.
- Stall detection, heartbeat timeout e brownout disable são não-negociáveis.
- Ver `docs/SERVO_SAFETY.md` para o protocolo completo de liberação.

### Memória

- **Nenhum framebuffer de display em SRAM.** Sprites LovyanGFX alocam em PSRAM.
- Buffers de áudio DMA: SRAM (verificar se DMA I2S alcança PSRAM no S3).
- Nenhum `malloc()` em caminho crítico (ISR, task de safety, render loop).
- Estruturas estáticas para event bus e pools de objetos frequentes.
- Monitorar `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` — manter >300KB livres (headroom câmera).

### Persistência e I/O

- **Nunca escrever em SD de forma síncrona em task com prioridade ≥ 10.**
- Toda escrita não-urgente vai para a fila da `persistence_task` (prioridade 5).
- Exceção: crash dump usa escrita síncrona direta (sistema já em falha).
- NVS para configuração crítica e flags de safety. SD para logs, assets, memória longa.

### Event Bus

- Ao mudar estado significativo: publicar evento no event bus.
- Nunca chamar subscriber diretamente para comunicação cross-layer.
- Eventos de safety têm fila separada e nunca são bloqueados por backpressure normal.

### WiFi e Conectividade

- **WiFi desabilitado nos Blocos 0–8.** Ativado na Etapa 9.6 via `wifi_service` (boot-time, background, Layer 2).
- O produto é **offline-first**: funciona 100% sem WiFi. Conectividade é conveniência, nunca dependência.
- Não assumir IP disponível em decisões de design anteriores à Etapa 9.6.
- Sem TLS/HTTPS no firmware: mbedTLS ~250 KB SRAM — inviável. HTTP local apenas.

---

## Estrutura de Componentes

```
components/
├── infra/          # Layer 2+3: boot_manager, logger, event_bus,
│                   #            config_manager, persistence_mgr,
│                   #            watchdog_service, error_policy, nb_events.h,
│                   #            motion_safety, power_monitor (Layer 3 físico)
├── nb_hal/         # Layer 1: display_hal (.cpp + .h), servo_hal,
│                   #          audio_hal, led_hal, touch_hal, sd_hal
├── services/       # Layer 4-5: render_service, motion_service,
│                   #             audio_service, led_service, touch_service,
│                   #             gaze_service, idle_service,
│                   #             expression_service, conductor
├── behavior/       # Layer 6: behavior_engine, state_machine, emotion_model
└── persona/        # Layer 7: persona_service, long_term_memory
```

---

## Convenções de Código

- Prefixo `nb_` para todos os tipos, funções e macros públicas do projeto.
- Arquivos de header com include guard `#ifndef NB_<MODULO>_H`.
- Erros retornam `esp_err_t`. Usar `ESP_ERROR_CHECK` apenas em init (não em runtime).
- Tasks: nome descritivo (`"nb_render_task"`), stack e prioridade documentados no header.
- Constantes de hardware (GPIO, limites de servo) em `nb_hal/nb_hw_config.h` — nunca hardcoded em lógica.

---

## Hardware Ativo (fase inicial)

| Periférico        | Interface   | Status     |
| ----------------- | ----------- | ---------- |
| ESP32-S3 N16R8    | —           | Ativo      |
| ST7789 2" display | SPI2        | Ativo      |
| WS2812 × 2        | RMT         | Ativo      |
| INMP441 mic       | I2S0 (RX)   | Ativo      |
| MAX98357A speaker | I2S1 (TX)   | Ativo      |
| SCS0009 × 2       | UART/FE-TTL | Ativo      |
| Touch (cobre)     | Touch GPIO  | Ativo      |
| microSD           | SPI3        | Ativo      |
| OV2640 câmera     | DVP         | **Adiado** |
| MPU-6050 IMU      | I2C0        | **Adiado** |
| LiPo + circuito   | —           | **Adiado** |

**Pinos DVP da câmera estão fisicamente conectados na placa. Nunca realocar esses GPIOs.**
Ver `docs/HARDWARE.md` para o mapa completo de pinos.

---

## Documentação de Referência

| Arquivo                | Conteúdo                                        |
| ---------------------- | ----------------------------------------------- |
| `docs/PROJECT.md`      | Visão geral, objetivos, princípios de produto   |
| `docs/ARCHITECTURE.md` | Camadas, componentes, event bus, tasks, memória |
| `docs/ROADMAP.md`      | Blocos e etapas detalhadas com critérios        |
| `docs/HARDWARE.md`     | Pinos, barramentos, restrições de GPIO          |
| `docs/PERSISTENCE.md`  | NVS vs SD, estrutura de diretórios, políticas   |
| `docs/ENERGY.md`       | Orçamento de energia, barramento 5V, brownout   |
| `docs/SERVO_SAFETY.md` | Parâmetros de safety, protocolo de liberação    |
