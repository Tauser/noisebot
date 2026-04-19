# NoiseBot — Roadmap

## Estrutura

O desenvolvimento é organizado em **Blocos** (agrupamentos temáticos) e **Etapas** (unidades de trabalho com critérios de aceitação verificáveis). Nenhuma etapa começa antes que as dependências estejam com todos os critérios verdes.

**Hierarquia de prioridades:** Fundação > Safety > Periféricos > Comportamento.

---

## BLOCO 0 — Fundação

> Objetivo: O sistema existe, inicia de forma determinística, se auto-protege e tem infraestrutura completa de logging, configuração e persistência.

---

### Etapa 0.1 — Boot Seguro, Logging e Watchdog

**Dependências:** Nenhuma (primeira etapa)
**Hardware necessário:** Sim (placa ESP32-S3 com UART conectado)

**O que entra:**

- `boot_manager`: fases de boot enumeradas, cada fase reporta sucesso/falha. Falha em fase crítica → safe mode.
- `logger`: nível configurável (VERBOSE/DEBUG/INFO/WARN/ERROR), saída UART0, prefixo com timestamp relativo, módulo e nível. Preparado para flush em SD (ativado na Etapa 0.3).
- `watchdog_service`: TWDT configurado para todas as tasks críticas. HW WDT com reset automático. Timeout de init (30s), timeout de operação (5s).
- `error_policy`: macros `NB_ASSERT(cond)`, `NB_ASSERT_FATAL(cond, msg)`, `NB_ASSERT_SAFETY(cond, msg)`.
- Safe mode flag em NVS: 3 boots com falha consecutivos → boot em safe mode.
- `sdkconfig.defaults`: CPU 240MHz, PSRAM octal, WiFi off, brownout on, TWDT on.

**O que fica de fora:** NVS completo (0.2), microSD (0.3), qualquer periférico de hardware.

**Critérios de aceitação:**

- [x] Boot completa em <2s sem periféricos conectados
- [x] Cada fase de boot aparece no log com timestamp e status OK/FAIL
- [x] Travar uma task propositalmente → sistema reseta em <WDT_TIMEOUT
- [x] 3 boots com falha simulada → boot seguinte logga "SAFE MODE" e desabilita motion
- [x] `NB_ASSERT_FATAL(false, "test")` → log de causa + reset
- [x] Reset reason (POWERON / BROWNOUT / WDT / SW) loggado em cada boot

---

### Etapa 0.2 — NVS e Config Manager

**Dependências:** 0.1 concluída
**Hardware necessário:** Sim (flash interna)

**O que entra:**

- `nvs_hal`: wrapper sobre `nvs_flash.h`, inicializa NVS, expõe read/write type-safe.
- `config_manager`: abstração sobre NVS. Namespaces: `nb_sys`, `nb_cfg`, `nb_svc`. Define todas as chaves em `nb_config_keys.h`. Load com defaults no primeiro boot. Validação de range em escrita.
- API exemplos: `config_get_servo_limit_min(servo_id)`, `config_set_volume(level)`, `config_get_touch_sensitivity()`.
- Defaults de safety: servo limits = centro ± 30° (expandir após calibração mecânica).

**Critérios de aceitação:**

- [x] Escrever config, resetar, ler: valor persistiu
- [x] Escrever valor fora de range: erro retornado, valor não persistido
- [x] `idf.py erase_flash` + boot: defaults aplicados, sem panic
- [x] Boot count incrementa a cada reset, zera em boot de sucesso (após Etapa 0.1)

---

### Etapa 0.3 — microSD, FATFS e Persistence Manager

**Dependências:** 0.2 concluída
**Hardware necessário:** Sim (microSD inserido)

**O que entra:**

- `sd_hal`: SPI3, mount FATFS, funções de leitura/escrita com tratamento de erro, unmount seguro.
- `persistence_mgr`: abstração unificada NVS + SD. Define contratos de armazenamento por categoria (ver `docs/PERSISTENCE.md`). `persistence_task` (Core 0, prioridade 5) consome fila de escritas não-urgentes.
- Estrutura de diretórios criada no primeiro mount: `logs/`, `assets/audio/`, `memory/`, `config/`, `snapshots/`.
- Log flush periódico para SD (a cada 60s ou ao entrar em SLEEPING).
- Boot sem SD: modo SD-degradado (só UART log, sem assets, sem LTM). Não panic.

**Critérios de aceitação:**

- [x] SD presente: mount OK, diretórios criados, confirmado via log
- [x] SD ausente no boot: logga "SD nao disponivel — modo degradado", sistema continua
- [x] Escrever 500 entradas de log: arquivo no SD tem tamanho correto
- [x] SD removido durante operação: erro loggado, modo degradado ativado, sem crash
- [x] Flush assíncrono: escrita via fila não bloqueia task de alta prioridade

---

### Etapa 0.4 — Power Monitor e Boot Safety

**Dependências:** 0.3 concluída
**Hardware necessário:** Sim (induzir brownout requer hardware)

**O que entra:**

- `power_monitor`: callback de brownout ESP-IDF registrado. Ao disparar: publica `NB_EVT_POWER_BROWNOUT_WARN`, solicita disable de torque (quando servos estiverem disponíveis), loga evento.
- Modos de operação do sistema:
  - `NB_POWER_NORMAL`: tudo habilitado
  - `NB_POWER_SD_DEGRADED`: SD ausente, logging só UART
  - `NB_POWER_SAFE_MODE`: motion desabilitado, só display + logs
  - `NB_POWER_EMERGENCY_STOP`: tudo desabilitado exceto logging
- `boot_manager` (expansão): sequência de resets por brownout → safe mode.

**Critérios de aceitação:**

- [x] Brownout simulado: callback dispara, evento publicado no bus, loggado
- [x] 3 brownouts consecutivos → próximo boot em safe mode
- [x] Em safe mode: servos não inicializam mesmo que código de servo esteja presente
- [x] Transição entre modos de operação loggada com motivo

---

### Etapa 0.5 — Event Bus

**Dependências:** 0.2 concluída (tipos de evento dependem de config)
**Hardware necessário:** Não (pode ser testado sem periféricos)

**O que entra:**

- Pool estático de 32 eventos (sem malloc por evento).
- Entrega síncrona (in-task) e assíncrona (cross-task via fila FreeRTOS).
- Fila separada para eventos de safety.
- Contador de eventos dropped → loggado a cada 10s se não-zero.
- Todos os `nb_event_type_t` definidos em `nb_events.h` (mesmo que sem handler ainda).

**Critérios de aceitação:**

- [x] 1000 publish/subscribe em loop: zero corrupção, zero leak de pool
- [x] Cross-task: publisher em prioridade alta, subscriber em prioridade baixa → entrega ocorre
- [x] Pool cheio: evento dropped contabilizado, sistema não trava
- [x] Unsubscribe funciona: evento não mais entregue após unsubscribe

---

### ✅ MARCO: BASE SÓLIDA CONCLUÍDA

Critérios adicionais de integração do Bloco 0:

- [x] Sistema estável por 1 hora sem periféricos externos: zero panics
- [x] Stack high watermark de todas as tasks medido e documentado
- [x] Heap usage estável (sem crescimento contínuo)
- [x] Safe mode: verificado de ponta a ponta com hardware

---

## BLOCO 1 — Display

> Objetivo: O robot tem face. Pipeline de renderização funcional com face procedural expressiva.

---

### Etapa 1.1 — LovyanGFX Bring-up

**Dependências:** Bloco 0 concluído
**Hardware necessário:** Sim (display ST7789 conectado)

**O que entra:**

- Componente `hal/display` com `display_lgfx_config.hpp`, `display_hal.cpp`, `display_hal.h`.
- CMakeLists com compilação mista C/C++.
- API C pura: `display_hal_init()`, `display_hal_set_brightness()`, `display_hal_fill()`, `display_hal_sprite_create/push/delete()`.
- Backlight via LEDC PWM (não GPIO direto).
- Pinos definidos em `nb_hw_config.h`.

**Critérios de aceitação:**

- [x] Display exibe cor sólida sem artefatos visuais
- [x] Backlight responde a `set_brightness(0..255)` com gradação suave (moditor nao tem gpio BL)
- [x] Dois threads chamando `display_hal_*` simultaneamente: sem corrupção
- [x] Sprite 240×240 alocado em PSRAM sem panic

---

### Etapa 1.2 — Framebuffer, Render Loop e Sprite System

**Dependências:** 1.1 concluída
**Hardware necessário:** Sim

**O que entra:**

- `render_service`: sprite principal 240×240 em PSRAM, `render_task` a 30-60fps.
- Sistema de layers: `render_service_register_layer(z_order, fn, ctx)`.
- Double buffering: sprites A/B em PSRAM, swap após DMA completo.
- Métricas: FPS real, tempo por layer, tempo de push SPI — loggados a cada 5s em DEBUG.

**Critérios de aceitação:**

- [x] FPS estável ≥ 30fps medido por 5 minutos contínuos
- [x] Nenhum artefato de tear durante updates de cor
- [x] Layer registrado/removido em runtime: sem flicker ou crash
- [x] `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)`: ≥ 300KB após alocação de sprites

---

### Etapa 1.3 — Face Procedural e Expressões Base

**Dependências:** 1.2 concluída
**Hardware necessário:** Sim

**O que entra:**

- `expression_service` (C++): renderer de olhos no estilo EMO — sem pupila, sem brow estrutural.
- `nb_face_state_t`: struct paramétrica com geometria por olho (cantos tl/tr/bl/br, abertura,
  squint, offset vertical/horizontal, curvatura, arredondamento, cor).
- 9 expressões base: NEUTRAL, HAPPY, CURIOUS, SLEEPY, FOCUSED, SUSPICIOUS,
  SURPRISED, SAD, ALARMED. Expressão vem exclusivamente do shape dos olhos.
- Boca e sobrancelhas: peças ocasionais de Layer 5+, não parte do modelo base.
- Interpolação linear entre dois `nb_face_state_t` (todos os 19 campos float + color step).
- Blink bilateral com distribuição de Poisson (µ=4.2s, min=1.6s). Olhos independentes:
  blink assimétrico ocasional (~20%) e wink suportados pelo modelo.
- Anti-aliasing sub-pixel nas bordas dos olhos (blend com fundo preto).

**Critérios de aceitação:**

- [x] 9 expressões renderizadas e visualmente distinguíveis por observador sem contexto
- [x] NEUTRAL forte e reconhecível: não parece olho genérico
- [x] Interpolação NEUTRAL → HAPPY em 300ms: suave, sem salto
- [x] Blink: 20 blinks observados, todos com timing diferente (distribuição Poisson)
- [x] Blink assimétrico: pelo menos 1 em 5 blinks com olhos ligeiramente dessincronizados
- [x] FPS mantido ≥ 30fps durante renderização de face completa

---

## BLOCO 2 — Periféricos Básicos

> Objetivo: LEDs e touch funcionais como canais de output e input.

---

### Etapa 2.1 — WS2812 LEDs

**Dependências:** Bloco 0 concluído  
**Hardware necessário:** Sim

**O que entra:**

- `led_hal`: RMT com driver `led_strip` do ESP-IDF.
- `led_service`: serviço não-bloqueante com `init`, `update(dt_ms)` e flush controlado para o HAL.
- API pública:
  - `led_set_color(idx, color)`
  - `led_set_all(color)`
  - `led_set_brightness(uint8_t)`
  - `led_fade_to(color, ms)`
  - `led_blink(count)`
  - `led_breathe(period_ms)`
- Controle por LED individual e em conjunto.
- Presets de estado:
  - boot (branco pulsante)
  - idle (quente baixo)
  - touch (flash quente)
  - safe mode (laranja)
  - error (vermelho pulsante)
- Camadas de comportamento:
  - `base state` persistente
  - `overlay effect` temporário com retorno automático ao estado base
- Prioridade entre estados:
  - `ERROR > SAFE_MODE > TOUCH > BOOT > IDLE`
- Transições não-bloqueantes com easing perceptual.
- Correção gamma para brilho visualmente mais linear.
- Limitador global de brilho/corrente para evitar pico desnecessário no barramento.
- Paleta nomeada/calibrada do projeto.
- Estrutura preparada para reuso pelo `idle_service` na Etapa 5.2.
- Patterns adicionais leves e reutilizáveis:
  - `flash_decay`
  - `heartbeat_pulse`
  - `solid`
  - `pulse`
- Respiração com pequena diferença de fase entre os 2 LEDs para evitar visual excessivamente mecânico.
- Timeout automático de efeitos transitórios.
- Otimização para só enviar frame ao WS2812 quando houver mudança real de estado.
- Modo noturno com brilho reduzido por configuração.

**Critérios de aceitação:**

- [x] Cores corretas em ambos os LEDs sem glitch de timing
- [x] `led_set_brightness(0..255)`: gradação visualmente suave
- [x] Fade de 0% a 100% em 500ms: visualmente linear
- [x] `led_breathe(4000)`: ciclo sinusoidal por 2 minutos sem drift
- [x] `flash_decay` de touch é visivelmente limpo e retorna ao estado base sem salto
- [x] `heartbeat_pulse` é distinguível de `breathe` e visualmente agradável
- [x] `touch` como efeito transitório retorna automaticamente ao estado base
- [x] `error` e `safe mode` sempre sobrepõem `idle`
- [x] Controle individual dos 2 LEDs funciona sem artefatos
- [x] Diferença de fase entre LEDs em idle permanece sutil e estável
- [x] Brilho máximo limitado: sem brownout/reset em teste contínuo de 5 minutos
- [x] Serviço roda sem bloquear render, touch ou loop principal
- [x] Frames não são reenviados inutilmente quando não há mudança visual
- [x] API e arquitetura reutilizáveis em 5.2 sem refator grande

**Implementação:** `components/nb_hal/led_hal.[c/h]` + `components/services/led_service/led_service.[c/h]`  
**GPIO LED DATA:** 19 (RMT canal 0, 2 LEDs em série)

---

### Etapa 2.2 — Touch Capacitivo

**Dependências:** Bloco 0 concluído  
**Hardware necessário:** Sim (fita de cobre conectada)

**O que entra:**

- `touch_hal`: uso do periférico touch do ESP32-S3 com polling fixo a `50 Hz`.
- Calibração de baseline no boot.
- `touch_service`: interpretação de toque em nível mais alto.
- Eventos mínimos:
  - `TAP` (<300ms)
  - `LONG_PRESS` (>800ms)
  - `SUSTAINED` (>3s)
  - `WAKE` (toque em `SLEEPING`)
- Threshold baseado em baseline × `SENSITIVITY_FACTOR`.
- `SENSITIVITY_FACTOR` persistido em NVS.
- Debounce de entrada para evitar ruído e falsos gatilhos.
- Histerese entre `touch_on` e `touch_off` para evitar chatter perto do threshold.
- Recalibração lenta de baseline quando o sistema estiver claramente sem toque.
- Proteção contra “baseline poisoning”:
  - não recalibrar enquanto houver toque ativo
  - não recalibrar durante ruído excessivo
- Estados internos do serviço:
  - `IDLE`
  - `TOUCHING`
  - `LONG_PRESSING`
  - `SUSTAINED_ACTIVE`
- Separação entre:
  - eventos one-shot (`TAP`, `LONG_PRESS`, `WAKE`)
  - estado contínuo (`is_touched`, `touch_duration_ms`)
- API de leitura para debug:
  - valor cru atual
  - baseline atual
  - threshold atual
  - estado atual
- Timeout de estabilização após boot para evitar falso toque na partida.
- Estrutura preparada para integração futura com:
  - LEDs (`touch flash`)
  - expressão facial
  - transições de estado do robô

**O que fica de fora nesta etapa:**

- gestos complexos
- multi-touch
- reconhecimento por posição
- sensor fusion com emoção/comportamento
- wake por interrupção profunda de energia
- ajuste automático avançado por contexto

**Critérios de aceitação:**

- [ ] TAP detectado em <20ms após toque
- [ ] 5 minutos sem toque: zero falsos positivos
- [ ] TAP vs LONG_PRESS: distinguíveis de forma confiável em 20 tentativas
- [ ] Operação simultânea de servos + touch: sem interferência perceptível
- [ ] Touch não entra em chatter ao ficar próximo do threshold
- [ ] Baseline permanece estável em repouso por 10 minutos
- [ ] Recalibração lenta compensa drift ambiental sem gerar falso evento
- [ ] Durante toque contínuo, baseline não deriva de forma a “engolir” o toque
- [ ] `WAKE` em estado `SLEEPING` funciona de forma confiável
- [ ] Métricas de debug (raw, baseline, threshold, state) refletem corretamente o comportamento observado
- [ ] Serviço roda sem bloquear render, LEDs ou loop principal

---

## BLOCO 3 — Motion Safety

> Objetivo: Servos seguros para uso progressivo. Nenhum movimento sem safety completo.

---

### Etapa 3.1 — Driver FE-TTLinker e Protocolo SCS

**Dependências:** Bloco 0 concluído
**Hardware necessário:** Sim (servos conectados via FE-TTLinker)
**Ação nesta etapa:** Apenas leitura. Nenhum movimento.

**O que entra:**

- `servo_hal`: UART1, protocolo SCSCL (header, ID, length, instruction, checksum).
- Instruções: PING, READ, WRITE.
- Leitura de registradores: posição atual, load, temperatura, voltagem.
- Retry em timeout: máx 3, depois retorna erro.
- PING ao boot para confirmar comunicação antes de qualquer comando.

**Critérios de aceitação:**

- [ ] PING para servo ID=1: OK
- [ ] PING para servo ID=2: OK
- [ ] READ posição atual: valor estável em servo parado
- [ ] READ temperatura: valor plausível (20–40°C em ambiente)
- [ ] Timeout de comunicação: retorna `ESP_ERR_TIMEOUT`, não trava

---

### Etapa 3.2 — Safety Layer de Motion

**Dependências:** 3.1 concluída
**Hardware necessário:** Sim (obrigatório verificar com hardware)
**Nota:** Esta etapa tem autoridade de veto sobre a Etapa 3.3. Nenhum movimento expressivo é liberado sem todos os critérios abaixo verificados.

**O que entra:**

- `motion_safety` (em `infra/` para evitar dependência circular):
  - Limites físicos por servo (NVS): rejeição de qualquer posição fora do range.
  - Limite de velocidade máxima (300 steps/s).
  - Monitoramento de load a 20Hz (`nb_safety_task`, Core 1, prioridade 23): WARN >40%, disable >70% por >100ms.
  - Monitoramento de temperatura: WARN ≥ 55°C, disable ≥ 70°C.
  - Heartbeat: `motion_task` reporta a cada 200ms. Timeout de 600ms → FAULT.
  - Disable ao brownout: subscreve `NB_EVT_POWER_BROWNOUT_WARN`, disable imediato.
  - Posição de parking (centro NVS) + disable de torque antes de qualquer disable.
  - Estados: DISABLED, INITIALIZING, ARMED, FAULT.
- `servo_hal` (adições): `servo_hal_write_position()`, `servo_hal_disable_torque()`.
- `boot_manager`: PHASE_SAFETY inicializa `motion_safety`; PHASE_MOTION arma os servos.
- Heartbeat keepalive temporário (`nb_hb_keep` task) — substituído pelo motion_service na Etapa 3.3.

**Critérios de aceitação (TODOS obrigatórios):**

- [ ] Comando fora de limite: rejeitado, loggado, servo não se move
- [ ] Stall simulado (bloquear servo com a mão): torque disablado em <150ms
- [ ] Heartbeat timeout simulado: torque disablado em <600ms
- [ ] Brownout simulado com servo em movimento: torque disablado antes do reset
- [ ] Temperatura simulada acima de crítico: disable e evento publicado
- [ ] 10 minutos de operação contínua de idle (sem movimento): temperatura estável
- [ ] Estado FAULT: sistema não autoriza ARMED novamente sem reset explícito

---

### Etapa 3.3 — Motion Primitivos e Interpolação

**Dependências:** 3.2 concluída e com TODOS os critérios verificados
**Hardware necessário:** Sim

**Implementado:**

- `motion_service` em `components/services/motion_service/`:
  - Interpolação cossenoidal (`cosine_ease`), motion_task 50Hz (Core 1, prioridade 20).
  - `motion_move_to(id, pos, ms)`: não-bloqueante via fila de comandos.
  - `motion_stop(id)`: para suavemente (congela na posição atual).
  - `motion_park_all()`: ambos os servos para centro em 500ms.
  - `motion_sequence_t`: player de keyframes com hold_ms.
  - Primitivos: `motion_neck_pan`, `motion_neck_tilt`, `motion_neck_look_at`.
  - Gestos: `motion_neck_nod`, `motion_neck_shake`, `motion_neck_tilt_curious`.
  - Safety injetada via `nb_motion_safety_iface_t` (sem dependência circular).
- `boot_manager`: PHASE_MOTION inicializa motion_service, arma servos, faz parking.
- `motion_safety_emergency_stop()`: API pública; usada por `NB_ASSERT_SAFETY` em
  error_policy.h para desabilitar torque antes de esp_restart() em violações de safety.

**Teste obrigatório antes de liberar para Bloco 5:**

- [ ] Cada primitivo executado 50 vezes: posição repetível, variação <2°
- [ ] Nenhum ruído mecânico excessivo durante movimento
- [ ] Temperatura estável após 50 ciclos
- [ ] Interpolação visualmente suave (sem saltos ou tremores)

---

## BLOCO 4 — Áudio

> Objetivo: Robot ouve e fala.

---

### Etapa 4.1 — INMP441: Microfone e VAD

**Dependências:** Bloco 0 concluído
**Hardware necessário:** Sim

**Implementado:**

- `audio_hal`: I2S0 full-duplex, Philips 32-bit estéreo, 16kHz. Extrai canal L (INMP441).
- `audio_service`: task "nb_audio_task" Core0 prio6. VAD por RMS sobre janelas de 256 samples (16ms).
- Threshold ajustável via `audio_service_set_vad_threshold()`. Default: 2000 (escala 24-bit).
- Eventos: `NB_EVT_VOICE_ACTIVITY_START`, `NB_EVT_VOICE_ACTIVITY_END` (após 300ms de silêncio).
- Gravação de diagnóstico: `audio_record_diagnostic(path, duration_s)` → WAV 16-bit mono no SD.

**Critérios de aceitação:**

- [x] Gravação de 3s: PCM audível sem artefatos (verificar via playback)
- [x] Falar perto do mic: `VOICE_ACTIVITY_START` em <200ms
- [x] Silêncio por 500ms: `VOICE_ACTIVITY_END` publicado

---

### Etapa 4.2 — MAX98357A: Playback de Áudio

**Dependências:** 4.1 concluída, SD com assets (Etapa 0.3)
**Hardware necessário:** Sim

**Implementado:**

- `audio_hal` (output): TX em I2S0 (full-duplex com mic). Streaming WAV em chunks de 256 amostras.
- `audio_service`: `audio_play_file(path)`, `audio_play_stop()`, `audio_set_volume(level)` (0–100).
- Streaming: lê 512 bytes/chunk do SD, sem carregar arquivo inteiro. Suporta arquivos >1MB.
- Volume: multiplicador digital aplicado ao PCM (level=0 → silêncio, level=100 → sem atenuação).
- SD_MODE tied HIGH externamente — sem controle SW (conforme nb_hw_config.h).
- Assets esperados em `/sdcard/assets/audio/`: greet_01–03, idle_01–03, touch_respond_01–03, error_01.
- Eventos: `NB_EVT_AUDIO_STARTED` (data.u32 = duration_ms), `NB_EVT_AUDIO_ENDED`.

**Critérios de aceitação:**

- [x] WAV 16kHz mono 16-bit: playback sem glitch
- [x] `audio_set_volume(0)`: silêncio (volume digital zerado)
- [x] Streaming de arquivo 1MB: sem OOM, sem glitch
- [x] Playback simultâneo com render de face ≥30fps: nenhum artefato em ambos

---

## BLOCO 5 — Comportamento

> Objetivo: Robot tem estado interno, gaze, idle convincente e outputs coordenados.

---

### Etapa 5.1 — State Machine e Emotion Model

**Dependências:** Bloco 0 concluído, 1.3 concluída
**Hardware necessário:** Parcialmente

**Implementado:**

- `state_machine` em `components/behavior/state_machine/`:
  - Estados: BOOT_UP, IDLE, ATTENTIVE, RESPONDING, TOUCH_REACTING, SLEEPING, ERROR, SAFE_MODE.
  - Idle timeout configurável via NVS (`config_get_idle_timeout_s()`).
  - TOUCH_REACTING retorna automaticamente a IDLE após 2s.
  - Todas as transições loggadas com timestamp, estado anterior, novo estado e motivo.
  - `nb_state_change_cb_t` publica `NB_EVT_STATE_CHANGED` no event bus via boot_manager.
  - Thread-safe via portMUX spinlock.
- `emotion_model` em `components/behavior/emotion_model/`:
  - Vetor (valência, ativação) em [-1, 1].
  - Decaimento exponencial: após ~60s decai a <5% do pico.
  - Mapeamento por nearest-neighbor a 9 anchors (um por expressão).
  - Eventos ajustam o vetor com deltas predefinidos.
  - Chama `expression_service_set()` quando a expressão discreta muda (transição 400ms).
  - Emoção persiste no NVS via `config_set_last_emotion()`.
- `behavior_task` ("nb_behav_task", Core 0, prio 5, 100ms): ticks de state_machine + emotion_model.
- boot_manager bridges: on_touch_event e on_audio_event chamam state_machine + emotion_model.

**Critérios de aceitação:**

- [ ] Todas as transições de estado loggadas com motivo e timestamp
- [ ] Emotion decai para neutral após 60s: verificado
- [ ] 9 emoções → 9 faces distinguíveis: verificado visualmente
- [ ] Timeout IDLE → SLEEPING: configurável via NVS, funcionando

---

### Etapa 5.2 — Gaze System e Idle Behavior

**Dependências:** 5.1 concluída, 3.3 concluída
**Hardware necessário:** Sim

**Implementado:**

- `gaze_service` em `components/services/gaze_service/`:
  - Render layer z=5 (Core 1, ~30fps) — antes do expression layer (z=10).
  - Saccade: fase rápida 60ms (linear + 10% overshoot) → settle 150ms (ease-out quadrático).
  - Micro-drift contínuo: passeio gaussiano low-pass, amplitude ≤ 0.06, retorno elástico.
  - API thread-safe: `gaze_service_set_target(x, y)` pode ser chamada de qualquer task.
  - Chama `expression_service_set_gaze()` a cada frame — offset aplicado no mesmo frame.
  - Nota: micro-tilt de pescoço é stub (depende de Etapa 3.3).
- `expression_service` (adição):
  - `expression_service_set_gaze(x, y)`: aplica translação bilateral dos olhos (±12px)
    e offset vertical (y_l/y_r), ambos sem afetar convergência (x_off) nem geometria de expressão.
- `idle_service` em `components/services/idle_service/`:
  - Micro-saccade a cada 5–15s (IDLE e ATTENTIVE): posições aleatórias, 20% de chance de retorno ao centro.
  - Aversive gaze a cada 8–15s (ATTENTIVE somente): desvia para o lado oposto à posição atual.
  - Yawn a cada 60–180s (IDLE somente): SLEEPY por 2.5s, retorno suave a NEUTRAL.
  - Gaze retorna ao centro ao sair de IDLE/ATTENTIVE.
  - Blink (Poisson, µ=4.2s) e LED breathing: gerenciados pelos próprios serviços.
  - Stub para micro-neck-movement (≤3/min, <5°): ativado na Etapa 3.3.

**Critério subjetivo obrigatório:**

- [ ] Observar robot em idle por 2 minutos: parece **vivo**, não mecânico

**Critérios mensuráveis:**

- [ ] Intervalo de blinks: nenhum <1.5s, nenhum >10s em observação de 5min
- [ ] Micro-movements de pescoço: amplitude <5°, frequência ≤ 3/minuto (stub — pós 3.3)
- [ ] Aversive gaze: olhar se desvia a cada 8-15s em modo ATTENTIVE

---

### Etapa 5.3 — Expression System

**Dependências:** 5.2 concluída
**Hardware necessário:** Sim

**Implementado:**

- `expression_play(expr, duration_ms, transition_ms)`: fila circular (cap=4),
  play temporário com retorno automático à base. `expression_service_set()` cancela
  play ativo (prioridade maior — emoção sempre vence expressão temporária).
- Mapeamento de eventos → expressões: touch e voz via emotion_model (direto, sem
  esperar tick da behavior_task → latência ≤1 frame ~33ms). Estado → expressão
  via state_machine + emotion_model já wired no boot_manager.
- Blink com curvas de Bezier: fechamento smoothstep (sigmoidal), abertura ease-out
  quadrático `(1-t)²` — olho abre rápido e desacelera ao final, mais natural.
- Idle yawn atualizado para usar `expression_play()` — retorna à emoção base
  correta (não hardcoded NEUTRAL).

**Critérios de aceitação:**

- [ ] Touch → expressão: latência percebida <100ms
- [ ] 15 blinks observados: nenhum idêntico em timing ao anterior
- [ ] NEUTRAL → HAPPY em 300ms: suave, sem frame perceptível de pulo
- [ ] Fila de expressões: 3 expressões enfileiradas, executadas em sequência

---

### Etapa 5.4 — Conductor

**Dependências:** 5.3 concluída, 4.2 concluída, 3.3 concluída
**Hardware necessário:** Sim (todos os subsistemas)

**O que entra:**

- `conductor`: API de ações de alto nível (`nb_action_t`). Partituras com keyframes temporais. Variações por ação (2-3 por ação). Interrupt suave ao receber nova ação.
- Ações iniciais: GREET, AGREE, DISAGREE, CURIOUS, TOUCH_WARM, TOUCH_STARTLE, SPEAK_LOOP, SLEEP, WAKE_UP.

**Implementado:**

- `conductor.c/.h` em `components/services/conductor/`: task "nb_conductor_t" prio 6, Core 0.
- 10 ações, até 3 variações cada. Sorteio via `esp_random()`.
- Partituras com keyframes de expressão + motion + áudio.
- Interrupt suave: flag `s_interrupt` verificada a cada 20ms no sleep interno.
- Wiring em `boot_manager.c`: touch TAP→TOUCH_WARM, LONG_PRESS→TOUCH_STARTLE, WAKE→WAKE_UP; estado SLEEPING→SLEEP, IDLE(de SLEEPING)→WAKE_UP; playback→SPEAK_LOOP; boot→GREET.

**Critério de qualidade (verificação com hardware):**

- [ ] Ação GREET: observador externo percebe face, motion e áudio como **uma** expressão unificada, não três outputs separados
- [ ] Ação SLEEP: transição gradual e suave (>2s de fade)
- [ ] Interrupt: nova ação enquanto ação em curso → transição limpa, sem movimento brusco

---

## BLOCO 6 — Integração e Memória

---

### Etapa 6.1 — Integração Completa

**Dependências:** Todos os blocos 0–5 concluídos
**Hardware necessário:** Sim

**O que entra:**

- Stress test de 1 hora com todos os subsistemas ativos.
- Profiling: CPU usage por task, stack high watermark, heap SRAM e PSRAM.
- Ajuste de prioridades e tamanhos baseado em dados reais.

**Implementado:**

- `render_service_get_fps()`: getter do último FPS medido (atualizado a cada 5s).
- `stats_dump()` em `boot_manager.c`: chamado a cada 60s pelo `behavior_task`.
  - Loga PSRAM free (KB), SRAM free (KB), FPS atual.
  - Watermark de stack por task: usa `xTaskGetHandle()` + `uxTaskGetStackHighWaterMark()`.
  - Tasks monitoradas: render, audio, motion, safety, conductor, behav, led, touch, persist, wdog.

**Critérios de aceitação (verificação com hardware):**

- [ ] FPS de render nunca abaixo de 25fps com áudio simultâneo
- [ ] Latência touch → resposta visual <100ms consistentemente em 50 tentativas
- [ ] Temperatura dos servos estável após 30min de uso normal
- [ ] Zero panics em 1 hora de operação contínua
- [ ] `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` ≥ 300KB ao final

---

### Etapa 6.2 — Memória de Longo Prazo

**Dependências:** 6.1 concluída
**Hardware necessário:** Sim

**O que entra:**

- `long_term_memory`: `interaction_history` (ring buffer 200 entradas, binário compacto), `persona_state` (binário com CRC), `event_journal` (1000 entradas rotativas), `usage_stats`.
- Flush para SD a cada 5min (behavior_task) ou ao entrar em SLEEPING.
- API: `ltm_get_total_touch_count()`, `ltm_get_hours_alive()`, `ltm_is_user_familiar()`.

**Implementado:**

- `components/persona/long_term_memory/`: ltm.h + ltm.c + CMakeLists.txt
- Dois arquivos SD (binário + CRC32): `ltm_main.bin` (~1.2KB) e `ltm_journal.bin` (~6KB)
- `ltm_main_file_t`: magic + version + total_touch_count + total_sessions + cumulative_uptime_s + familiarity_score + ring buffer 200 entradas + crc32
- `ltm_journal_file_t`: magic + version + ring buffer 1000 entradas + crc32
- Familiar: `score = 1 − exp(−touches/50)` → familiar (score ≥ 0.5) a partir de ~35 toques
- Wiring em `boot_manager.c`:
  - touch TAP/LONG → `ltm_record(LTM_IACT_TOUCH_*)`
  - touch WAKE → `ltm_record(LTM_IACT_WAKE)`
  - voice start → `ltm_record(LTM_IACT_VOICE_START)`
  - playback → `ltm_record(LTM_IACT_AUDIO_PLAYED)`
  - SLEEPING → `ltm_record(LTM_IACT_SLEEP)` + `ltm_flush()`
  - IDLE (de SLEEPING) → `ltm_record(LTM_IACT_WAKE)`
  - behavior_task: `ltm_tick(100)` a cada tick, `ltm_flush()` a cada 5min

**Critérios de aceitação (verificação com hardware):**

- [ ] Após 100 interações simuladas: dados persistidos corretamente no SD
- [ ] `ltm_is_user_familiar()` muda de false para true após threshold de interações
- [ ] Corrupção simulada de arquivo de memória: sistema re-inicializa arquivo, não para
- [ ] Flush assíncrono: não bloqueia tasks de prioridade ≥ 10

---

## BLOCO 7 — Polimento de Produto

---

### Etapa 7.1 — Performance e Memória

- Otimizar render pipeline (gargalos de SPI DMA)
- Auditar fragmentação de heap após 8h de operação
- Ajustar buffers baseado em uso real
- Documentar limites operacionais com dados reais

### Etapa 7.2 — Refinamento Comportamental

- Calibrar timings de idle com observação humana
- Refinar partituras do conductor
- Adicionar variações suficientes para evitar padrão repetitivo perceptível
- Calibrar responsividade de touch com usuário real

### Etapa 7.3 — Testes de Produto

- 8 horas contínuas: zero panics, temperatura estável
- 100 power cycles: boot consistente, NVS íntegro
- SD removal/insertion em operação
- Brownout simulado em todas as fases
- Stall de servo em todas as posições de range

---

## BLOCO 8 — Expansões de Hardware

> Objetivo: Ativar periféricos adiados que já estão fisicamente conectados na placa.

---

### Etapa 8.1 — Câmera OV2640

**Dependências:** Bloco 7 concluído, 300KB PSRAM headroom verificado
**Hardware necessário:** Sim (FPC câmera conectado — pinos já reservados)

**O que entra:**

- `camera_hal`: driver ESP-IDF esp32-camera, DVP, QVGA (320×240), 15fps.
- Frame buffer único em PSRAM (~150KB). Sem double-buffer — não é display, é análise.
- API: `camera_hal_capture()`, `camera_hal_get_frame()`, `camera_hal_release_frame()`.
- Task de captura separada: Core 1, prio 4 (abaixo de safety e render).

**Critérios de aceitação:**

- [ ] Frame capturado sem artefatos: verificado via dump para SD
- [ ] PSRAM após alocação de framebuf: ≥ 300KB livre
- [ ] FPS de render mantido ≥ 30fps com câmera ativa
- [ ] Zero interferência com audio (I2S0) e display (SPI2)

---

### Etapa 8.2 — IMU MPU-6050

**Dependências:** 8.1 concluída (I2C0 iniciado pela câmera, endereço 0x68 disponível)
**Hardware necessário:** Sim (MPU-6050 conectado via I2C0 — GPIO 4/5)

**O que entra:**

- `imu_hal`: driver I2C0 para MPU-6050. Leitura de acelerômetro + giroscópio a 50Hz.
- Tap detection via DMP do MPU-6050 (interrupt em GPIO spare ou polling).
- `NB_EVT_IMU_TAP`: batida física detectada pelo acelerômetro.
- `NB_EVT_IMU_SHAKE`: agitação detectada (threshold em NVS).
- Detecção de pouso: robot colocado na mesa após ser carregado → greet.

**Critérios de aceitação:**

- [ ] Tap na mesa: `NB_EVT_IMU_TAP` publicado em <50ms
- [ ] Shake (sacudir o robot): `NB_EVT_IMU_SHAKE` publicado
- [ ] 5 minutos de leitura contínua: zero interferência com render e áudio
- [ ] I2C compartilhado com câmera (0x3C e 0x68): sem colisão de barramento

---

### Etapa 8.3 — Bateria e Gestão de Energia

**Dependências:** Hardware de bateria presente (nova versão de placa)
**Hardware necessário:** bq25185 (0x6B), MAX17048 (0x36), TPS61088 (boost 5V)

**O que entra:**

- `battery_hal`: driver I2C para bq25185 (charger) e MAX17048 (fuel gauge).
- `power_manager` (expansão): modos de economia com níveis de bateria.
  - `BATTERY_LOW` (< 20%): desabilita LEDs, reduz FPS para 20, reduz idle_timeout.
  - `BATTERY_CRITICAL` (< 5%): entra em SLEEPING imediatamente, flush LTM.
- `NB_EVT_BATTERY_LOW`, `NB_EVT_BATTERY_CRITICAL`, `NB_EVT_CHARGING_STARTED`.
- Robot expressa estado de bateria via expressão (SLEEPY quando baixo).

**Critérios de aceitação:**

- [ ] Nível de bateria lido corretamente (±5% vs multímetro)
- [ ] `BATTERY_LOW`: comportamento de economia ativado
- [ ] `BATTERY_CRITICAL`: sistema dorme antes de corte de energia
- [ ] Carga detectada: expressão de contentamento ao plugar carregador

---

## BLOCO 9 — Completude da Stack

> Objetivo: Criar os serviços previstos na arquitetura que não existem, e consolidar
> infraestrutura de observabilidade e agendamento. Zero hardware novo — tudo software
> sobre periféricos já ativos.

---

### Etapa 9.1 — diagnostics_service (Layer 2) ✅

**Dependências:** Bloco 6 concluído
**Hardware necessário:** Não

**O que entra:**

- `diagnostics_service` em `components/infra/diagnostics_service/`:
  - Consolida o `stats_dump()` avulso do `boot_manager` num serviço com API.
  - Coleta: FPS atual, PSRAM/SRAM livres, stack watermark por task, uptime.
  - `health_score` composto (0–100): degrada com heap baixo, FPS baixo, stack alto.
  - Eventos: `NB_EVT_HEALTH_WARNING` (score < 50), `NB_EVT_HEAP_LOW` (PSRAM < 300KB).
  - `diagnostics_dump_to_sd()`: snapshot completo em `/sdcard/logs/diag_<uptime>.txt`.
  - Chamado pela behavior_task a cada 60s — substitui o stats_dump() atual.

**API:**

```c
esp_err_t diagnostics_init(void);
uint8_t   diagnostics_get_health_score(void);
uint32_t  diagnostics_get_fps(void);
uint32_t  diagnostics_get_heap_free(uint32_t caps);
uint32_t  diagnostics_get_uptime_s(void);
void      diagnostics_dump_to_sd(void);
```

**Critérios de aceitação:**

- [x] `health_score` reflete degradação real: simular heap baixo → score cai
- [x] `NB_EVT_HEAP_LOW` publicado quando PSRAM < 300KB
- [x] Dump em SD gerado corretamente com todas as métricas
- [x] Remove `stats_dump()` do boot_manager sem perda de funcionalidade

---

### Etapa 9.2 — schedule_service (Layer 5) ✅

**Dependências:** 9.1 concluída
**Hardware necessário:** Não

**O que entra:**

- `schedule_service` em `components/services/schedule_service/`:
  - Lista de timers one-shot e recorrentes, verificada no tick da behavior_task.
  - Sem task própria: `schedule_service_tick(dt_ms)` chamado pelo behavior_task.
  - `schedule_after(delay_ms, cb, ctx)` → retorna handle.
  - `schedule_repeat(interval_ms, cb, ctx)` → repete até cancelado.
  - `schedule_cancel(handle)`.
  - Capacidade estática: 16 slots (sem malloc).
  - Migrar `s_alone_timer_ms` do idle_service para schedule_service após implementação.

**API:**

```c
esp_err_t       schedule_service_init(void);
void            schedule_service_tick(uint32_t dt_ms);
schedule_handle_t schedule_after(uint32_t delay_ms, schedule_cb_t cb, void *ctx);
schedule_handle_t schedule_repeat(uint32_t interval_ms, schedule_cb_t cb, void *ctx);
void            schedule_cancel(schedule_handle_t h);
```

**Critérios de aceitação:**

- [x] `schedule_after(1000, cb, NULL)`: cb chamado após 1000 ± 100ms
- [x] `schedule_repeat(500, cb, NULL)`: cb chamado 10 vezes em 5s, intervalo estável
- [x] 16 schedules simultâneos: nenhum drop
- [x] Cancel: cb não chamado após cancelamento
- [x] Zero malloc em todo o caminho crítico

---

### Etapa 9.3 — behavior_engine (Layer 6) ✅

**Dependências:** 9.2 concluída, persona_service (11.1) pode ser adicionado depois
**Hardware necessário:** Não

**O que entra:**

- `behavior_engine` em `components/behavior/behavior_engine/`:
  - Tabela de regras declarativa: `{trigger_event, condition_fn, actions[]}`.
  - Avaliada a cada evento publicado no bus — substitui os `switch/case` do boot_manager.
  - `condition_fn`: ponteiro de função com acesso a estado, emoção e persona.
  - `actions[]`: lista de ações atômicas (PLAY_CONDUCTOR, EMIT_EMOTION, LTM_RECORD, etc.).
  - Prioridade de regra: regra mais específica (com condition) vence regra genérica.
  - Regras registradas em tabela estática (sem malloc).
  - boot_manager mantém apenas init e wiring de HAL callbacks — toda lógica migra para aqui.

**Exemplo de regra:**

```c
{ .trigger  = NB_EVT_TOUCH_TAP,
  .cond     = cond_trust_high,      /* persona_get_trust() > 0.6 */
  .actions  = { ACT_EMOT(TOUCH_TAP), ACT_CONDUCTOR(TOUCH_WARM), ACT_LTM(TOUCH_TAP) } },

{ .trigger  = NB_EVT_TOUCH_TAP,
  .cond     = NULL,                 /* fallback sem condition */
  .actions  = { ACT_EMOT(TOUCH_TAP), ACT_CONDUCTOR(TOUCH_STARTLE), ACT_LTM(TOUCH_TAP) } },
```

**Critérios de aceitação:**

- [x] Todos os comportamentos do boot_manager replicados via regras
- [x] boot_manager.c reduzido: sem switch/case de comportamento — só HAL callbacks e init
- [x] Regra com condition vs fallback: condition avaliada corretamente
- [x] 20 regras simultâneas: sem overhead mensurável no tick

---

### Etapa 9.4 — sound_analysis_service (Layer 4) ✅

**Dependências:** audio_service ativo (Etapa 4.1)
**Hardware necessário:** Não (mic já funcionando)

**O que entra:**

- `sound_analysis_service` em `components/services/sound_analysis_service/`:
  - FFT de 256 pontos sobre janelas PCM do mic (16ms por janela a 16kHz).
  - Classificação: `SILENCE`, `VOICE`, `MUSIC`, `NOISE`, `CLAP`, `WHISTLE`.
  - `CLAP`: 2 picos RMS broadband em <400ms. `WHISTLE`: pico narrowband 1–3kHz, >300ms.
  - Publicação de eventos no bus.
  - API de query para serviços de Layer 5+ que precisam de nível e classe atual.

**Eventos adicionados em `nb_events.h`:**

```c
NB_EVT_SOUND_CLAP,          /* palmas detectadas */
NB_EVT_SOUND_WHISTLE,       /* assobio detectado */
NB_EVT_SOUND_MUSIC_START,   /* música ambiente detectada */
NB_EVT_SOUND_MUSIC_END,
NB_EVT_SOUND_CLASS_CHANGED, /* transição de classe (data: nova classe) */
```

**API:**

```c
esp_err_t         sound_analysis_init(void);
void              sound_analysis_tick(const int16_t *pcm, size_t samples);
nb_sound_class_t  sound_analysis_get_class(void);
float             sound_analysis_get_rms(void);
float             sound_analysis_get_dominant_freq(void);
```

**Critérios de aceitação:**

- [x] Silêncio por 30s: classe `SILENCE` estável, zero falsos positivos
- [x] Palmas 3x: `NB_EVT_SOUND_CLAP` publicado em cada evento, <100ms de latência
- [x] Assobio de 1s: `NB_EVT_SOUND_WHISTLE` detectado
- [x] Música tocando no ambiente: `MUSIC` dentro de 3s, `MUSIC_END` após parar
- [x] FFT + classificação: ≤ 200µs por janela de 256 samples (medido com esp_timer)

---

### Etapa 9.5 — synth_service (Layer 4) ✅

**Dependências:** audio_service ativo (Etapa 4.2), mutex sobre I2S0
**Hardware necessário:** Não (speaker já funcionando)

**O que entra:**

- `synth_service` em `components/services/synth_service/`:
  - Geração procedural de áudio: sine, square, sawtooth, white noise filtrado.
  - Envelope ADSR por voz (attack, decay, sustain, release).
  - Primitivos expressivos com parâmetros aleatorizados: nenhum som idêntico.
  - Compartilha I2S0 com `audio_service` via mutex — não toca simultaneamente com WAV.
  - Timbres emocionais mapeados às expressões do emotion_model.

**Primitivos:**

```c
synth_chirp(float freq_start, float freq_end, uint32_t duration_ms);
synth_purr(uint32_t duration_ms, float intensity);        /* noise filtrado com LFO */
synth_blip(float freq, uint32_t duration_ms);             /* tom curto com decay */
synth_melody(const nb_note_t *notes, uint8_t count);      /* sequência de notas */
synth_set_timbre(nb_synth_timbre_t t);                    /* SINE/SQUARE/SAW/NOISE */
synth_play_for_emotion(nb_expression_t expr);             /* mapeamento automático */
```

**Mapeamento emoção → timbre:**

- `HAPPY/CURIOUS` → chirps ascendentes, freq 600–1200Hz
- `SAD` → tons descendentes, freq 200–400Hz, decay longo
- `ALARMED` → burst de noise + freq alta rápida
- `SLEEPY` → sinusoide lenta, freq 100–200Hz, fade lento

**Critérios de aceitação:**

- [x] `synth_chirp(400, 1200, 200)`: tom audível e ascendente sem distorção
- [x] `synth_purr(3000, 0.8)`: ronronar contínuo suave por 3s
- [x] `synth_play_for_emotion(NB_EXPR_HAPPY)`: som reconhecível como alegre por ouvinte
- [x] 10 chamadas ao mesmo primitivo: nenhuma idêntica (randomização de parâmetros)
- [x] Sem glitch ao alternar entre synth e WAV playback (mutex respeitado)

---

### Etapa 9.6 — WiFi Service (Layer 2)

**Dependências:** 9.1 concluída (diagnostics_service)
**Hardware necessário:** Não (WiFi nativo do ESP32-S3)

**O que entra:**

- `wifi_service` em `components/infra/wifi_service/`:
  - Conecta ao AP configurado em NVS (`nb.wifi.ssid`, `nb.wifi.pass`) durante o boot.
  - Inicialização **não-bloqueante**: `wifi_service_init()` retorna imediatamente; associação ocorre em background via eventos `esp_wifi`.
  - Sem credenciais em NVS: loga WARN e permanece desconectado — sistema opera normalmente offline.
  - Reconexão automática com backoff exponencial (1s → 2s → 4s → max 60s).
  - mDNS: hostname `noisebot.local` registrado após IP adquirido.
  - Eventos publicados no bus:
    ```c
    NB_EVT_WIFI_CONNECTED,       /* associou ao AP */
    NB_EVT_WIFI_IP_ACQUIRED,     /* IP obtido via DHCP — serviços podem iniciar */
    NB_EVT_WIFI_DISCONNECTED,    /* conexão perdida */
    ```
  - `wifi_service_get_ip()`: retorna IP atual como string (ou vazio se desconectado).
  - `wifi_service_is_connected()`: query síncrona thread-safe.

**NVS keys:**

| Chave             | Tipo   | Default |
| ----------------- | ------ | ------- |
| `nb.wifi.ssid`    | string | (vazio) |
| `nb.wifi.pass`    | string | (vazio) |
| `nb.wifi.enabled` | bool   | true    |

**Critérios de aceitação:**

- [ ] Sem credenciais: boot completo normalmente, log "WiFi sem credenciais — offline"
- [ ] Com credenciais: `NB_EVT_WIFI_IP_ACQUIRED` publicado, `noisebot.local` resolve no browser
- [ ] AP cai durante operação: `NB_EVT_WIFI_DISCONNECTED` publicado, reconexão automática em < 70s
- [ ] FPS de render mantido ≥ 25fps durante associação inicial ao WiFi
- [ ] SRAM livre após init: ≥ 80 KB confirmado via `diagnostics_dump`

---

## BLOCO 10 — Inteligência Sensorial

> Objetivo: Transformar inputs já existentes (mic, touch) em fontes de informação
> semântica. O robot passa a distinguir o "o quê" e o "como" dos estímulos,
> não apenas a presença/ausência deles.

---

### Etapa 10.1 — attention_service (Layer 5)

**Dependências:** 9.2 (schedule_service), 9.4 (sound_analysis_service)
**Hardware necessário:** Não

**O que entra:**

- `attention_service` em `components/services/attention_service/`:
  - Modelo unificado de atenção como float contínuo [0.0, 1.0].
  - Substitui a lógica binária `is_idle || is_attentive` em idle_service e gaze_service.
  - Fontes de atenção com pesos configuráveis via NVS.
  - Decaimento exponencial por ausência de estímulo (τ = 30s).

**Fontes e pesos default:**

```
VOICE_START   → +0.70  (máxima atenção)
TOUCH_TAP     → +0.50
SOUND_CLAP    → +0.40
SOUND_LOUD    → +0.20  (rms alto, classe VOICE)
SOUND_MUSIC   → +0.15  (presença ambiental suave)
```

**Consumidores:**

- `gaze_service`: velocidade de saccade proporcional a `attention_level`.
- `idle_service`: yawn threshold proporcional a `1.0 - attention_level`.
- `expression_service`: duração de transição inversamente proporcional a attention.
- `state_machine`: ainda controla transições de estado (atenção é camada ortogonal).

**API:**

```c
esp_err_t attention_service_init(void);
void      attention_service_on_stimulus(nb_attention_source_t src, float intensity);
float     attention_service_get_level(void);
void      attention_service_tick(uint32_t dt_ms);
```

**Critérios de aceitação:**

- [x] Sem estímulo por 60s: nível decai de 1.0 para < 0.1
- [x] `VOICE_START`: nível sobe para ≥ 0.7 imediatamente
- [x] Nível 0.1 vs 0.9: diferença visual percebida em velocidade de gaze e pálpebra
- [x] Atenção não altera transições de estado da state_machine

---

### Etapa 10.2 — rhythm_service (Layer 5)

**Dependências:** 9.4 (sound_analysis_service)
**Hardware necessário:** Não

**O que entra:**

- `rhythm_service` em `components/services/rhythm_service/`:
  - Detecção de BPM por autocorrelação sobre envelope RMS (janelas de 500ms).
  - Range válido: 60–180 BPM. Abaixo ou acima: classificado como `NO_RHYTHM`.
  - Confiança do BPM (0.0–1.0): publicar `NB_EVT_RHYTHM_LOCKED` ao atingir > 0.7.
  - `NB_EVT_BEAT_TICK`: publicado a cada beat detectado quando `RHYTHM_LOCKED`.
  - `NB_EVT_RHYTHM_LOST`: quando confiança cai abaixo de 0.3 por > 2s.

**Consumidores:**

- `led_service`: `BEAT_TICK` → flash suave nos LEDs no ritmo.
- `conductor`: quando `RHYTHM_LOCKED` e estado IDLE → head-bob sutil a cada beat.
- `idle_service`: música detectada → suspende yawn (não boceja durante música).

**API:**

```c
esp_err_t rhythm_service_init(void);
void      rhythm_service_tick(uint32_t dt_ms);
float     rhythm_service_get_bpm(void);
float     rhythm_service_get_confidence(void);
bool      rhythm_service_is_locked(void);
```

**Critérios de aceitação:**

- [x] Música a 120 BPM: BPM detectado dentro de ±5 após 5s
- [x] LEDs pulsam visivelmente no ritmo por observador sem contexto
- [x] Música parada: `RHYTHM_LOST` em < 3s
- [x] Fala humana: `NO_RHYTHM` (não confunde fala com ritmo)

---

### Etapa 10.3 — VAD Semântico (Layer 4/5)

**Dependências:** 9.4 (sound_analysis_service), 9.2 (schedule_service)
**Hardware necessário:** Não (mic já funcionando)

**O que entra:**

Extensão do `audio_service` + wiring no `boot_manager`/`behavior_engine`:

- **Análise de duração de fala**: fala > 4s → `NB_EVT_VOICE_LONG` (discurso); fala < 0.5s → `NB_EVT_VOICE_SHORT` (interjeição).
- **Análise de energia da fala**: RMS durante VOICE_START categorizado. `VOICE_LOUD` (> 2× baseline) vs `VOICE_SOFT` (< 0.5× baseline).
- **Timer pós-voz**: 8–12s após `VOICE_END` sem nova interação → `NB_EVT_VOICE_FOLLOWUP_TIMEOUT` (cadê você?).
- **Reação por padrão**: `VOICE_SHORT` repetido 3× em < 30s → `NB_EVT_VOICE_REPEATED` (impaciência detectada).

**Eventos adicionados:**

```c
NB_EVT_VOICE_SHORT,              /* interjeição < 500ms */
NB_EVT_VOICE_LONG,               /* discurso > 4s */
NB_EVT_VOICE_LOUD,               /* fala intensa */
NB_EVT_VOICE_SOFT,               /* fala suave */
NB_EVT_VOICE_FOLLOWUP_TIMEOUT,   /* silêncio pós-voz */
NB_EVT_VOICE_REPEATED,           /* interjeições repetidas */
```

**Comportamentos via behavior_engine:**

- `VOICE_LONG` → nod de concordância (conductor: `NB_ACTION_AGREE`).
- `VOICE_SHORT` × 3 → head-tilt confuso (nova ação no conductor).
- `VOICE_FOLLOWUP_TIMEOUT` → gaze busca origem do som (gaze sweep lateral).
- `VOICE_LOUD` → ALARMED momentâneo.
- `VOICE_SOFT` → CURIOUS + gaze inclina levemente.

**Critérios de aceitação:**

- [x] Fala de 5s: `VOICE_LONG` publicado ao final
- [x] 3 interjeições em 25s: `VOICE_REPEATED` publicado
- [x] 10s após voz parar: `VOICE_FOLLOWUP_TIMEOUT` publicado, gaze sweep visível
- [x] Fala em tom alto: expressão muda para ALARMED/SURPRISED por ≤ 2s
- [x] Fala suave: CURIOUS ativado

---

### Etapa 10.4 — Touch Semântico (Layer 4/5)

**Dependências:** 9.2 (schedule_service), 9.3 (behavior_engine)
**Hardware necessário:** Não (touch já funcionando)

**O que entra:**

Extensão do `touch_service` + novas regras no `behavior_engine`:

- **SUSTAINED com progressão emocional**: toque > 3s acumula calor crescente.
  - 3–8s: delta emocional médio a cada 1s (`TOUCH_TAP` × 0.5/s).
  - > 8s: `NB_EVT_TOUCH_DEEP` — emoção máxima, `synth_purr()` ativado.
- **Sequência de TAPs**: 2 TAPs em < 500ms → `NB_EVT_TOUCH_DOUBLE_TAP` (knock-knock).
- **Detecção de carinho**: SUSTAINED por > 15s contínuos → `NB_EVT_TOUCH_CARESS`.

**Eventos adicionados:**

```c
NB_EVT_TOUCH_DEEP,         /* toque sustentado longo (>8s) */
NB_EVT_TOUCH_DOUBLE_TAP,   /* dois taps rápidos */
NB_EVT_TOUCH_CARESS,       /* carinho prolongado >15s */
```

**Comportamentos:**

- `TOUCH_DEEP` → purr via synth_service + emoção máxima de calor.
- `TOUCH_DOUBLE_TAP` → SURPRISED + ação de susto leve (nova partitura conductor).
- `TOUCH_CARESS` → expressão especial (HAPPY fechado, satisfeito) + ronronar longo.

**Critérios de aceitação:**

- [x] Toque de 10s: purr audível a partir de 8s, emoção muda visivelmente
- [x] 2 taps em 400ms: `TOUCH_DOUBLE_TAP` publicado, sem dois `TAP` separados
- [x] Toque de 20s: `TOUCH_CARESS` publicado, expressão satisfeita visível
- [x] Progressão suave: sem salto de expressão durante SUSTAINED

**Implementação:** `touch_semantic_service` (Layer 5) em
`components/services/touch_semantic_service/`. Wired via `on_touch_event` no
`boot_manager` (10.4 em `behavior_task`). Eventos adicionados a `nb_events.h`.
Regras no `behavior_engine` e deltas no `emotion_model` completos.

---

## BLOCO 11 — Personalidade Emergente

> Objetivo: O robot evolui ao longo do tempo. Cada sessão deixa traços que
> modificam sutil mas perceptivelmente o comportamento futuro. A memória de
> longo prazo passa de observadora passiva a motor do caráter.

---

### Etapa 11.1 — persona_service (Layer 7)

**Dependências:** long_term_memory (6.2), behavior_engine (9.3)
**Hardware necessário:** Não

**O que entra:**

- `persona_service` em `components/persona/persona_service/`:
  - Lê LTM e deriva 4 dimensões contínuas [0.0, 1.0] persistidas em NVS.
  - Atualizado a cada boot e a cada flush do LTM (comportamento emergente, não reativo).
  - Expõe API de query para behavior_engine, conductor, idle_service, emotion_model.

**Dimensões e fórmulas:**

```
warmth    = 1 − exp(−touch_count/50)        /* familiar = caloroso */
energy    = clamp(voice_sessions/total × 2)  /* mais voz = mais energético */
curiosity = clamp(1 − sleep_ratio × 1.5)     /* dorme menos = mais curioso */
trust     = min(warmth, sessions/20)         /* combina familiaridade + tempo */
```

**Consumidores (via behavior_engine conditions):**

- `trust > 0.6`: TAP → `TOUCH_WARM` (não mais `TOUCH_STARTLE` como padrão).
- `warmth > 0.7`: GREET → variação entusiasmada (partitura diferente no conductor).
- `curiosity > 0.6`: idle_service aumenta frequência de micro-saccades em 50%.
- `energy > 0.7`: emotion transitions 30% mais rápidas.
- `trust < 0.3`: novos sons → ALARMED em vez de CURIOUS.

**API:**

```c
esp_err_t persona_service_init(void);
void      persona_service_refresh(void);     /* recalcular a partir do LTM */
float     persona_get_warmth(void);
float     persona_get_energy(void);
float     persona_get_curiosity(void);
float     persona_get_trust(void);
```

**Critérios de aceitação:**

- [x] Após 0 toques: `warmth` < 0.1, GREET usa variação tímida
- [x] Após 35 toques: `warmth` ≥ 0.5 (`ltm_is_user_familiar()` true), GREET usa variação calorosa
- [x] Observador externo: comportamento após 100 interações perceptivelmente diferente do boot inicial
- [x] Dimensões persistem em NVS: reiniciar sem SD não reseta a personalidade

---

### Etapa 11.2 — Ritmos Circadianos (Layer 5)

**Dependências:** 9.2 (schedule_service), long_term_memory (uptime)
**Hardware necessário:** Não

**O que entra:**

Extensão de `idle_service` + `schedule_service` para ciclo de uptime:

- **Fase de dia simulado**: dividido em 3 fases por uptime acumulado na sessão.
  - `DAWN` (0–30min): robot recém acordado, movimentos lentos, mais sonolento.
  - `DAY` (30min–4h): operação normal, energia plena.
  - `DUSK` (> 4h): yawns mais frequentes, idle_timeout 30% menor, LEDs mais quentes.
- **Despertar gradual**: na fase DAWN, blink mais lento, gaze mais lento, transições mais longas.
- **Cansaço acumulado**: `DUSK` com sessions altas → idle_timeout ainda menor.
- Fase persiste via LTM: sessões curtas frequentes vs sessões longas raras modulam energia inicial.

**Comportamentos:**

- DAWN: GREET mais suave e lento (nova variação de partitura).
- DUSK: yawn a cada 30–60s (vs 60–180s normal), micro-saccades mais lentos.
- Transição DAWN → DAY: stretch (nova ação no conductor — nod + expressão HAPPY).

**Critérios de aceitação:**

- [x] 0–30min de uptime: yawns ocorrem com menor frequência e menor intensidade
- [x] > 4h de uptime: yawn a cada ≤ 60s; idle_timeout claramente mais curto
- [x] Transição DAY: "stretch" visível uma vez ao sair de DAWN
- [x] LTM registra fases: uptime por sessão loggado e usado na sessão seguinte

---

### Etapa 11.3 — Micro-Expressões e Combinações (Layer 5)

**Dependências:** expression_service (5.3), synth_service (9.5), schedule_service (9.2)
**Hardware necessário:** Não

**O que entra:**

- **Micro-expressões**: flashes emocionais de 80–150ms antes da expressão principal.
  - Touch TAP → flash de SURPRISED (80ms) → HAPPY (expressão principal).
  - Voz forte → flash de ALARMED (120ms) → CURIOUS.
  - GREET → flash de SURPRISED (100ms) → HAPPY estendido.
- **Expressões compostas**: `expression_combo_play(seq[], count)` — fila com micro-intervalos.
- **Expressões involuntárias**: com probabilidade baixa (5%), substituições de expressão por reação não esperada.
  - Em HAPPY prolongado → piscar rápido duplo (satisfação).
  - Em FOCUSED → micro-squint (concentração extra).

**API:**

```c
void expression_combo_play(const nb_expr_frame_t *frames, uint8_t count);

typedef struct {
    nb_expression_t expr;
    float           duration_ms;
    float           transition_ms;
} nb_expr_frame_t;
```

**Critérios de aceitação:**

- [x] TAP: micro-SURPRISED visível (< 100ms) antes de HAPPY — observador percebe diferença vs sem micro-expressão
- [x] 20 touch events observados: pelo menos 3 micro-expressões distintas (não sempre a mesma)
- [x] `expression_combo_play` de 3 frames: sequência executada sem saltos

---

### Etapa 11.4 — Modos Especiais de Comportamento (Layer 5/6)

**Dependências:** 11.1 (persona_service), 9.5 (synth_service), 10.1 (attention_service)
**Hardware necessário:** Não

**O que entra:**

- **Modo Meditação**: ativado por TOUCH_SUSTAINED (>5s) em IDLE.
  - LEDs: respiração muito lenta (6s ciclo), cor âmbar suave.
  - Display: expressão serena próxima a NEUTRAL com pálpebras levemente pesadas.
  - Áudio: white noise suave procedural via synth.
  - Gaze: drift muito lento, sem saccades.
  - Atenção desativada: VOICE_START não interrompe (só touch sai do modo).
  - Estado: `NB_STATE_MEDITATION` adicionado à state_machine.

- **Modo Companhia Silenciosa**: ativado automaticamente após 2h em IDLE sem interação.
  - Semelhante à meditação mas sem trigger manual.
  - LEDs: heartbeat muito suave, quase imperceptível.
  - Robot simplesmente "está presente" sem comportamentos ativos.
  - Qualquer interação retorna ao IDLE normal.

- **Celebrações de Marco**: eventos únicos não-repetíveis disparados via LTM.
  - 50º toque: ação especial no conductor (HAPPY prolongado + sons de celebração).
  - 100h de uptime: greet especial ao acordar naquele dia.
  - Registrados no LTM para nunca repetir.

**Critérios de aceitação:**

- [x] SUSTAINED > 5s em IDLE: modo meditação ativo em < 500ms
- [x] Modo meditação: observador percebe ambiente calmo distinto do IDLE normal
- [x] Saída de meditação: só via touch, voz não interrompe
- [x] 50º toque: celebração única ocorre — não se repete no 51º

---

## BLOCO 12 — Bridge LLM

> Objetivo: Conectar o robot a LLMs externos via bridge local (RPi/PC na mesma rede),
> preservando o princípio offline-first — o robot funciona completamente
> sem o bridge; com ele, ganha capacidade conversacional.

---

### Etapa 12.1 — Protocolo Bridge (Layer 2)

**Dependências:** Bloco 9 concluído, **9.6 concluída** (wifi_service ativo)
**Hardware necessário:** Não

**Transporte:** TCP primário, UART fallback automático.

O bridge roda em RPi ou PC na mesma rede local — sem cabo USB obrigatório.

**O que entra:**

- `bridge_service` em `components/infra/bridge_service/`:
  - Task "nb_bridge_task": Core 0, prio 4, 4KB stack.
  - Framing idêntico nos dois transportes: `[0xAB][len_16][type_8][payload][crc8]`.
  - Seleção de transporte automática no boot (ver lógica abaixo).

**Seleção de transporte:**

```
boot
 └─ WiFi IP adquirido?
     ├─ SIM → aguarda conexão TCP na porta 9000 por 2s
     │         ├─ cliente conectou → modo TCP (loggado)
     │         └─ timeout         → tenta handshake UART (200ms)
     │                              ├─ bridge responde → modo UART
     │                              └─ sem resposta   → offline
     └─ NÃO → tenta handshake UART imediatamente (200ms)
               ├─ bridge responde → modo UART
               └─ sem resposta   → offline
```

- Reconexão: se TCP cair durante operação, tenta reconectar a cada 5s por 60s; após isso, permanece offline até próximo boot.
- Apenas um transporte ativo por vez.

**TCP:**
- Servidor no ESP32, porta 9000.
- Keep-alive TCP: detecta queda em < 10s.

**UART (fallback de desenvolvimento):**
- UART0/USB CDC, 921600 baud, separado do log serial.
- Útil durante desenvolvimento sem AP configurado.

**Mensagens ESP32 → Bridge:**

```
AUDIO_CHUNK   payload: pcm_raw int16[], 512 samples (32ms)
EVENT         payload: nb_event_type_t + data
STATUS        payload: state, emotion vec, attention level, health score
```

**Mensagens Bridge → ESP32:**

```
SAY           payload: wav_chunk int16[] (streaming em chunks de 512)
EXPR          payload: nb_expression_t + duration_ms
ACTION        payload: nb_action_t
EMOT_EVENT    payload: nb_emotion_event_t
GAZE          payload: float x, float y
TEXT_SCROLL   payload: string (para futuro display de texto)
```

**Critérios de aceitação:**

- [x] Boot sem bridge e sem WiFi: sistema opera normalmente, sem bloqueio visível
- [x] Bridge TCP conecta: handshake em < 300ms, modo TCP loggado
- [x] Bridge UART conecta (WiFi off): handshake em < 200ms, modo UART loggado
- [ ] AUDIO_CHUNK stream via TCP: jitter < 10ms entre chunks em rede local  ← validar em HW
- [x] TCP cai durante conversa: sistema detecta em < 10s, retorna offline
- [x] CRC8 com erro: frame descartado, contabilizado, sem crash

---

### Etapa 12.2 — Pipeline LLM via Bridge

**Dependências:** 12.1 concluída, bridge (RPi/PC) com Whisper + Gemini + Piper instalados
**Hardware necessário:** Raspberry Pi 4 ou PC na mesma rede local

**O que entra:**

No ESP32:

- Wiring no behavior_engine: `VOICE_START` → inicia stream de AUDIO_CHUNK para bridge.
- `VOICE_END` → envia marcador de fim de fala.
- Recebe `SAY` → toca via audio_service (streaming de chunks).
- Recebe `EXPR`/`ACTION`/`EMOT_EVENT` → injeta nos serviços correspondentes.
- Timeout: se bridge não responde em 8s após `VOICE_END` → retorna comportamento offline.

No bridge (fora do firmware, script Python/Node):

- Descobre ESP32 via mDNS (`noisebot.local:9000`) — sem IP hardcoded.
- Recebe chunks de áudio → buffer até `VOICE_END`.
- Whisper (local, small model) → transcrição.
- Gemini Flash free tier com system prompt de persona do NoiseBot.
- Response parser: extrai intenção + emoção + texto de resposta.
- Piper TTS → WAV → envia em chunks via `SAY`.
- Envia `EXPR` e `ACTION` conforme intenção da resposta.

**System prompt do NoiseBot (base):**

- Personalidade do robot conforme persona_service (warmth/trust/energy).
- Contexto de estado atual (estado, emoção, uptime, familiaridade).
- Respostas curtas (< 10s de fala), nunca explicativas — sempre expressivas.

**Critérios de aceitação:**

- [ ] Pergunta simples ("tudo bem?"): resposta em < 8s do início da fala
- [ ] Resposta chega em chunks: áudio começa antes do WAV completo (streaming)
- [ ] `EXPR` e `ACTION` chegam: face e motion coordenados com a fala
- [ ] Bridge offline mid-conversation: robot expressa confusão (CURIOUS) e retorna a idle
- [ ] 10 conversas consecutivas: zero crash, sem degradação de memória

---

## BLOCO 13 — Visão por Computador

> Objetivo: Usar a câmera (8.1) para detecção de presença, face tracking e
> gestos simples. Sem ML pesado — algoritmos clássicos dentro dos limites
> do ESP32-S3.

---

### Etapa 13.1 — Detecção de Presença (Layer 4)

**Dependências:** 8.1 (câmera), 10.1 (attention_service)
**Hardware necessário:** Câmera OV2640

**O que entra:**

- `vision_service` em `components/services/vision_service/`:
  - Frame differencing: compara frame atual com frame de referência.
  - Presence score: % de pixels alterados acima de threshold.
  - `NB_EVT_PRESENCE_DETECTED`, `NB_EVT_PRESENCE_LOST`.
  - Frame de referência atualizado a cada 30s de ausência.

**Integração:**

- Substitui o timer cego do `NB_EMOT_EVT_IDLE_LONG` por confirmação visual de ausência.
- `PRESENCE_DETECTED` após `PRESENCE_LOST` → greet mais entusiasmado (via persona + behavior_engine).
- `attention_service_on_stimulus(VISION_PRESENCE, intensity)`.

**Critérios de aceitação:**

- [ ] Pessoa entra no campo de visão: `PRESENCE_DETECTED` em < 500ms
- [ ] Pessoa sai e fica ausente 2min: `PRESENCE_LOST` publicado
- [ ] Iluminação variável (sombra passando): zero falsos positivos de `PRESENCE_DETECTED`
- [ ] FPS de render: mantido ≥ 25fps com vision_service ativo

---

### Etapa 13.2 — Face Tracking (Layer 4/5)

**Dependências:** 13.1 concluída
**Hardware necessário:** Câmera OV2640

**O que entra:**

- Extensão de `vision_service`:
  - Detecção de rosto por segmentação de cor pele (YCbCr thresholds) + análise de forma.
  - Sem CNN/ML — algoritmo clássico que cabe em < 50KB de código.
  - Output: posição normalizada do rosto detectado (-1.0 a 1.0 em x e y).
  - Confiança da detecção (0.0–1.0).
  - `NB_EVT_FACE_DETECTED` (data: posição), `NB_EVT_FACE_LOST`.

**Integração:**

- `gaze_service`: quando `FACE_DETECTED` com confiança > 0.5, gaze segue o rosto suavemente.
- `attention_service`: `FACE_DETECTED` → máxima atenção (1.0).
- `idle_service`: sem face detectada + IDLE_LONG → solidão confirmada (SAD mais rápido).

**Critérios de aceitação:**

- [ ] Rosto detectado a 30–50cm: gaze se move visivelmente em direção ao rosto
- [ ] Rosto se move lateralmente: gaze acompanha com delay < 200ms
- [ ] Sem rosto: `NB_EVT_FACE_LOST` em < 2s após saída do frame
- [ ] Falso positivo (parede, objeto): < 1 por minuto em ambiente típico

---

### Etapa 13.3 — Detecção de Gestos Simples (Layer 4/5)

**Dependências:** 13.2 concluída
**Hardware necessário:** Câmera OV2640

**O que entra:**

- Extensão de `vision_service`:
  - Detecção de mão aberta por análise de contorno (região clara próxima ao rosto detectado).
  - Gestos suportados: `WAVE` (mão em movimento lateral), `OPEN_HAND` (mão parada aberta).
  - `NB_EVT_GESTURE_WAVE`, `NB_EVT_GESTURE_OPEN_HAND`.

**Comportamentos:**

- `GESTURE_WAVE` → GREET (como se a pessoa acenasse).
- `GESTURE_OPEN_HAND` parado por > 2s → robot fica parado esperando (FOCUSED).

**Critérios de aceitação:**

- [ ] Acenar 5 vezes: ≥ 4 `GESTURE_WAVE` detectados
- [ ] Mão aberta parada: FOCUSED ativado em < 1s
- [ ] Objeto não-mão: < 1 falso positivo por minuto

---

## BLOCO 15 — Conectividade

> Objetivo: O robot expõe uma interface web local para configuração, monitoramento
> e controle remoto. WiFi já está ativo desde a Etapa 9.6 — este bloco constrói
> os serviços de aplicação sobre essa infraestrutura.

**Restrições de hardware para este bloco (ESP32-S3):**
- TLS/HTTPS: mbedTLS consome ~250 KB SRAM adicionais — inviável. HTTP na LAN apenas.
- Máximo 1 cliente WebSocket simultâneo.
- Sem streaming de áudio ou vídeo via WiFi (jitter e banda insuficientes).

**Orçamento de SRAM incremental (além do wifi_service da 9.6):**

| Componente             | SRAM estimada |
| ---------------------- | ------------- |
| esp_http_server (4 cx) | ~20 KB        |
| WebSocket (1 cliente)  | ~10 KB        |
| **Total incremental**  | **~30 KB**    |

---

### Etapa 15.1 — Web Dashboard e Companion API (Layer 2)

**Dependências:** 9.6 concluída (IP adquirido)
**Hardware necessário:** Não

**O que entra:**

- `web_service` em `components/infra/web_service/`:
  - `esp_http_server` com máximo 4 conexões HTTP simultâneas.
  - Arquivos estáticos servidos do SD (`/sdcard/www/`): `index.html`, `app.js`, `style.css`.
    - Se `/sdcard/www/` ausente: endpoint `/` retorna página mínima embutida no firmware.
  - WebSocket em `/ws`: 1 cliente simultâneo; novo cliente desconecta o anterior.
  - Iniciado somente após `NB_EVT_WIFI_IP_ACQUIRED` — nunca bloqueia o boot.

**REST API:**

| Endpoint              | Método | Descrição                                          |
| --------------------- | ------ | -------------------------------------------------- |
| `GET /`               | HTTP   | Dashboard (HTML do SD ou fallback embutido)         |
| `GET /api/status`     | HTTP   | JSON: state, expression, attention, health, uptime, fps |
| `GET /api/persona`    | HTTP   | JSON: warmth, energy, curiosity, trust             |
| `GET /api/config`     | HTTP   | JSON com todas as chaves NVS relevantes            |
| `POST /api/config`    | HTTP   | Atualiza chave NVS (body: `{"key":"val","value":x}`) |
| `POST /api/command`   | HTTP   | Injeta ação (body: `{"type":"ACTION","value":"GREET"}`) |
| `WS /ws`              | WS     | Push de status a cada mudança; aceita comandos     |

**WebSocket (push do robot):**

```json
{ "type": "status", "state": "IDLE", "expression": "NEUTRAL",
  "attention": 0.3, "health": 87, "uptime_s": 3612, "fps": 45 }
```

**WebSocket (comando do cliente):**

```json
{ "type": "command", "action": "GREET" }
{ "type": "emot_event", "event": "TOUCH_TAP" }
```

**Sem autenticação no protótipo** (LAN local, sem exposição externa).

**Critérios de aceitação:**

- [x] Browser em `http://noisebot.local`: dashboard carrega em < 3s
- [x] `GET /api/status`: JSON válido retornado em < 100ms
- [x] WebSocket: status push recebido em < 200ms após mudança de estado no robot
- [x] `POST /api/command` GREET: `conductor_play(GREET)` executado em < 300ms
- [x] `POST /api/config` volume: `config_set_volume()` persistido e efetivo sem reiniciar
- [x] FPS de render ≥ 25fps com cliente WS conectado e recebendo updates
- [x] Cliente WS desconecta abruptamente: sem crash, nova conexão aceita normalmente

---

### Etapa 15.2 — OTA e Backup de Personalidade

**Dependências:** 15.1 concluída
**Hardware necessário:** Não

**O que entra:**

- **OTA via HTTP** usando WiFi já ativo da Etapa 9.6:
  - Endpoint `POST /api/ota` recebe URL de firmware `.bin` (servidor local ou S3).
  - `esp_ota` com validação de magic bytes antes de aplicar.
  - Robot entra em `NB_STATE_OTA` durante update: motion off, LEDs laranja pulsante, WS push de progresso.
  - Rollback automático se firmware não confirmar boot em 30s (`esp_ota_mark_app_valid_cancel_rollback()`).
  - Sem TLS: URL deve ser HTTP (limitação de RAM — ver nota no cabeçalho do bloco).

- **Backup/restore de personalidade via web**:
  - `GET /api/persona/export`: JSON com LTM snapshot + dimensões NVS (warmth/energy/curiosity/trust).
  - `POST /api/persona/import`: restaura a partir do JSON — permite migrar personalidade para novo hardware.
  - Mesmo formato exportável via cópia direta do SD (`/sdcard/memory/ltm_main.bin`).

**Critérios de aceitação:**

- [x] OTA aplicado via `POST /api/ota`: firmware atualizado, NVS preservado
- [x] OTA com firmware corrompido: rollback ocorre, firmware anterior ativo no próximo boot
- [x] Durante OTA: FPS de render cai para 0 (estado OTA suspende render), LEDs indicam progresso
- [x] Export JSON: arquivo válido, importável em outro hardware com comportamento idêntico
- [x] WiFi permanece ativo após OTA (não desliga mais após update)


---

## BLOCO 16 — Voz e Expressividade Avançada

> Objetivo: Robot fala, exibe emoções visuais ricas e executa sequências coreografadas. Inspirado em features do StackChan (wake word, TTS, blush/coração, dança).

---

### Etapa 16.1 — Wake Word Service (Layer 4)

**Dependências:** 10.3 (VAD Semântico) concluída
**Hardware necessário:** INMP441 já conectado

**O que entra:**

- `wake_word_service` (Layer 4): detecta keyword configurável usando `esp_afe` (Audio Front End) + `esp_mn` (MultiNet) do ESP-SR.
  - Keyword padrão: "Noise Bot" (customizável via NVS).
  - Pipeline: INMP441 → `esp_afe` (beamforming + noise suppression) → `esp_mn` (keyword spotting).
  - Roda em task dedicada (prioridade 8), consome ~80KB PSRAM para modelos.
- Publica `NB_EVT_WAKE_WORD` no event bus ao detectar keyword com confiança ≥ threshold configurável.
- `boot_manager` registra handler: `NB_EVT_WAKE_WORD` → `conductor_play(NB_ACTION_WAKE_UP)` se em SLEEPING, senão `GREET`.
- NVS: `nb_svc/ww_enabled` (u8), `nb_svc/ww_threshold` (u8, 0–100).
- Integrado ao web dashboard: enable/disable, threshold slider.

**Critérios de aceitação:**

- [ ] Dizer "Noise Bot" com robot em SLEEPING → acorda e cumprimenta em <1.5s
- [ ] Dizer "Noise Bot" em IDLE → plays GREET
- [ ] Ruído ambiente sem keyword: zero false positives em 5 minutos
- [ ] `ww_enabled=0` em NVS: keyword não detectada, VAD continua normal
- [ ] Threshold ajustável pelo dashboard sem reflash

---

### Etapa 16.2 — TTS Service (Layer 4)

**Dependências:** 4.2 (audio playback), 9.6 (WiFi), 12.1 (bridge HTTP) concluídas
**Hardware necessário:** MAX98357A já conectado

**O que entra:**

- `tts_service` (Layer 4): sintetiza texto em fala via servidor TTS local HTTP.
  - Protocolo: `POST http://<host>:<port>/tts` com body `{"text":"...","speaker_id":0}`, resposta WAV/PCM streaming.
  - Servidores suportados (mesma API): VOICEVOX, Coqui TTS, AquesTalk.
  - Streaming: `esp_http_client` + pipe de chunks para `audio_service` → sem buffer completo em RAM.
  - Task dedicada (prioridade 6), stack 6KB.
- API pública: `tts_service_speak(const char *text, uint8_t priority)`.
  - Prioridade 0 = interruptível, prioridade 1 = completa antes de aceitar novo pedido.
  - Publica `NB_EVT_AUDIO_STARTED` / `NB_EVT_AUDIO_ENDED` compatíveis com pipeline existente.
- NVS: `nb_tts/host`, `nb_tts/port`, `nb_tts/speaker_id`, `nb_tts/enabled`.
- Web dashboard: seção TTS — host/porta/speaker, botão de teste com campo de texto.
- Conductor: novo action `NB_ACTION_SPEAK_GREETING` usa TTS se disponível, senão synth_service.

**Critérios de aceitação:**

- [ ] `tts_service_speak("olá, mundo", 0)` → robot fala via speaker em <2s (WiFi local)
- [ ] TTS durante expressão: face continua animada enquanto fala
- [ ] TTS sem WiFi ou servidor offline: fallback para synth_service sem panic
- [ ] Dois pedidos simultâneos: segundo aguarda fila, não sobrescreve
- [ ] Volume do TTS responde ao `config_get_volume()` existente

---

### Etapa 16.3 — Overlays de Expressão: Blush, Coração e Breath (Layer 5)

**Dependências:** 5.3 (expression_service), 10.4 (touch semântico) concluídas
**Hardware necessário:** Não

**O que entra:**

- **Blush overlay**: manchas rosadas semi-transparentes nas bochechas, desenhadas sobre qualquer expressão base.
  - `expression_service_overlay_blush(uint8_t intensity, uint32_t duration_ms)` — intensity 0–255, fade-out automático.
  - Ativado automaticamente: CARESS → blush máximo, TOUCH_DEEP → blush médio, TOUCH_WARM_PULSE → blush leve.
  - Renderizado como dois círculos com alpha blending no sprite PSRAM.

- **Heart overlay**: ❤️ animado temporário no centro da tela.
  - `expression_service_overlay_heart(uint32_t duration_ms)` — escala in/out suave.
  - Ativado por: duplo toque CARESS consecutivo (>15s), ou `NB_ACTION_CELEBRATE`.
  - Desenhado como shape paramétrico (dois arcos + triângulo), sem assets externos.

- **Breath animation** (idle): pulsação sutil de abertura dos olhos imitando respiração.
  - `idle_service` aplica senoide lenta (período 4–6s, amplitude ±4% de `open_l/r`) quando em IDLE/ATTENTIVE.
  - Sincronizado ao circadiano: DAWN = mais lento (6s), DAY = normal (5s), DUSK = mais lento (6s).
  - Desabilitável via NVS `nb_svc/breath_enabled`.

**Critérios de aceitação:**

- [ ] CARESS 15s → blush aparece, dura 5s, faz fade-out sem cortar expressão base
- [ ] `NB_ACTION_CELEBRATE` → heart overlay visível por 2s
- [ ] Breath animation: ciclo visível a olho nu, não interfere com blink automático
- [ ] Blush + expressão HAPPY simultaneamente: ambos visíveis no display
- [ ] Desativar breath via NVS: idle sem pulsação de olhos

---

### Etapa 16.4 — Choreography Player (Layer 5/6)

**Dependências:** 5.4 (conductor), 16.3 concluídas
**Hardware necessário:** Não (servos opcionais para coreografias completas)

**O que entra:**

- `nb_choreo_t`: struct `{nb_action_t action; uint16_t delay_ms; uint8_t flags}`.
  - Flag `NB_CHOREO_PARALLEL`: executa próximo step sem aguardar o atual terminar.
  - Flag `NB_CHOREO_WAIT_AUDIO`: aguarda fim do TTS/synth antes de avançar.
- `conductor_play_choreo(const nb_choreo_t *steps, uint8_t count)`: executa sequência, publicando eventos de progresso.
- `conductor_stop_choreo()`: interrompe sequência em andamento.
- Coreografias built-in (constantes):
  - `NB_CHOREO_DANCE`: sequência de 8 ações com timing rítmico.
  - `NB_CHOREO_WAKE_UP_RITUAL`: STRETCH → YAWN → HAPPY → blush leve (integra 16.3).
  - `NB_CHOREO_GREETING_ELABORATE`: GREET → CURIOUS → AGREE → heart (>5s de interação).
- Web dashboard: seção "Choreography" com 3 botões de play + campo de sequência custom em JSON.
- API `/api/choreo` (POST): `{"steps":[{"action":"GREET","delay_ms":500},...]}`

**Critérios de aceitação:**

- [ ] `NB_CHOREO_DANCE` executada: 8 ações na ordem correta, timing dentro de ±50ms
- [ ] Choreo interrompível: `conductor_stop_choreo()` para no step atual sem travar conductor
- [ ] Choreo via web dashboard: POST JSON → sequência executa no robot
- [ ] `NB_CHOREO_FLAG_PARALLEL`: dois steps sem delay visível entre eles
- [ ] Choreo com TTS pendente + `WAIT_AUDIO`: step seguinte aguarda fim da fala

---

## BLOCO 17 — LLM com Ação Física

> Objetivo: O LLM passa de "gerador de texto" para "agente com corpo" — pode expressar, mover, acender LEDs e falar enquanto responde.

---

### Etapa 17.1 — LLM Function Calling (Layer 6/7)

**Dependências:** 12.2 (LLM Bridge), 16.2 (TTS), 16.4 (Choreo) concluídas
**Hardware necessário:** Não

**O que entra:**

- Extensão do `llm_bridge` (Etapa 12.2) com suporte a tool use / function calling.
- Schema de tools enviado ao LLM no system prompt (formato compatível com OpenAI/Ollama tool_calls):
  - `play_expression(expression: string)` → `expression_service_set()`
  - `play_action(action: string)` → `conductor_play()`
  - `play_choreo(name: string)` → `conductor_play_choreo()`
  - `set_led(r, g, b)` → `led_set_all()`
  - `speak(text: string)` → `tts_service_speak()`
  - `get_status()` → retorna JSON de estado atual
- Parser de `tool_calls` no response do LLM: extrai chamadas, executa via dispatcher interno, retorna resultado ao LLM para continuar a resposta.
- Execução paralela texto + ação: LLM pode gerar texto de resposta e simultâneamente acionar expressão.
- Timeout de tool execution: 3s por tool, fallback silencioso se falhar.
- NVS: `nb_llm/tools_enabled` (u8, default 1).

**Critérios de aceitação:**

- [ ] LLM recebe pergunta → responde texto + executa `play_expression(HAPPY)` simultaneamente
- [ ] LLM invoca `speak()` → robot fala a frase gerada via TTS
- [ ] LLM invoca `play_choreo(dance)` → coreografia executa durante resposta textual
- [ ] Tool call com parâmetro inválido: logged, ignorado, resposta textual continua
- [ ] `tools_enabled=0`: LLM responde normalmente sem tools

---

## BLOCO 18 — Modos de Tela

> Objetivo: Além da face expressiva, o display pode mostrar fotos e transmitir câmera ao vivo. Depende do Bloco 8 (câmera e hardware expandido).

---

### Etapa 18.1 — Photo Frame Mode (Layer 5/6)

**Dependências:** 8.1 (câmera opcionalmente), 0.3 (microSD) concluídas
**Hardware necessário:** microSD com fotos em `/sdcard/photos/`

**O que entra:**

- Novo estado `NB_STATE_PHOTO_FRAME` no `state_machine`.
- `photo_frame_service` (Layer 6): carrega arquivos JPEG do SD via FATFS, decodifica com LovyanGFX `drawJpgFile()`, exibe em loop com transição suave (fade via overlay PSRAM).
  - Intervalo configurável: NVS `nb_svc/photo_interval_s` (default 30s).
  - Suporta até 256 arquivos indexados no boot.
  - Retorna automaticamente para IDLE após toque ou voz detectada.
- Ativação: toque longo (>3s) em modo SLEEPING → entra em PHOTO_FRAME.
- `NB_ACTION_PHOTO_FRAME` adicionado ao conductor.
- Web dashboard: botão "Photo Frame" + upload de JPEG via `POST /api/photos`.

**Critérios de aceitação:**

- [ ] 10 fotos no SD → exibidas em loop, intervalo correto
- [ ] Toque durante slideshow → retorna para IDLE em <500ms
- [ ] JPEG inválido ou corrompido → skipped, próxima foto sem panic
- [ ] `photo_interval_s` alterado via dashboard sem reflash
- [ ] Upload de foto via `/api/photos` → aparece no próximo ciclo do slideshow

---

### Etapa 18.2 — Camera MJPEG Stream (Layer 2/4)

**Dependências:** 8.1 (câmera OV2640), 15.1 (web_service) concluídas
**Hardware necessário:** câmera OV2640 conectada no DVP

**O que entra:**

- Endpoint `GET /stream` no `web_service`: MJPEG multipart stream (Content-Type: `multipart/x-mixed-replace`).
  - Frame rate: até 15fps QVGA (320×240), limitado por WiFi throughput.
  - Resolução configurável: QVGA (default) ou QQVGA via query param `?res=qqvga`.
  - 1 cliente simultâneo (igual ao WS); novo cliente desconecta o anterior.
- `GET /api/camera/snapshot`: JPEG único, útil para polling de baixa frequência.
- `POST /api/camera/config`: `{"resolution":"QVGA","quality":10}` — ajusta encoder JPEG.
- Integração com `face_tracking_service` (Etapa 13.2): quando ativo, stream inclui overlay de bounding box da face detectada (via header HTTP custom `X-Face-Detected: 1`).
- Web dashboard: seção "Camera" com tag `<img src="/stream">` e botão snapshot.

**Critérios de aceitação:**

- [ ] `http://noisebot.local/stream` exibe vídeo ao vivo no browser sem plugin
- [ ] 15fps sustentado por 60s sem OOM ou watchdog
- [ ] Snapshot: JPEG válido retornado em <500ms
- [ ] Stream ativo não degrada FPS do display (render_service isolado)
- [ ] Segundo cliente: primeiro é desconectado em <1s

---

## Resumo de Marcos

| Marco                | Bloco          | Indicador                                                       |
| -------------------- | -------------- | --------------------------------------------------------------- |
| BASE SÓLIDA          | Fim do Bloco 0 | Boot determinístico, watchdog, NVS, SD, event bus               |
| DISPLAY PRONTO       | Etapa 1.3      | Face EMO com 9 expressões, blink assimétrico, FPS ≥ 30          |
| MOTION SAFE          | Etapa 3.2      | Todos os critérios de safety verificados                        |
| ROBOT EXPRESSIVO     | Etapa 5.4      | Conductor funcionando, outputs coordenados                      |
| PRODUTO INICIAL      | Etapa 6.1      | 1h sem panic, latência OK, temperatura OK                       |
| PRODUTO MADURO       | Etapa 7.3      | 8h contínuas, 100 power cycles, testes de produto               |
| HARDWARE EXPANDIDO   | Etapa 8.3      | Câmera, IMU e bateria ativos e integrados                       |
| STACK COMPLETA       | Etapa 9.6      | Todos os serviços da arquitetura existem, WiFi ativo            |
| OUVIDOS INTELIGENTES | Etapa 10.4     | Robot distingue tipo, tom e padrão de estímulos                 |
| PERSONALIDADE VIVA   | Etapa 11.4     | Comportamento perceptivelmente diferente após 1 semana de uso   |
| ROBOT CONVERSADOR    | Etapa 12.2     | Conversa completa com LLM: fala → entende → responde → expressa |
| ROBOT VIDENTE        | Etapa 13.3     | Olha para quem está na frente, reage a gestos                   |
| ROBOT CONECTADO      | Etapa 15.2     | Dashboard web ativo, OTA funcional, personalidade portável      |
| ROBOT EXPRESSIVO+    | Etapa 16.4     | Fala, ruboriza, dança — expressividade completa                 |
| ROBOT AGENTE         | Etapa 17.1     | LLM aciona hardware durante resposta — age enquanto pensa       |
| ROBOT VISUAL         | Etapa 18.2     | Câmera ao vivo no browser, fotos no display                     |
