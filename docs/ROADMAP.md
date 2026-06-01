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

- [x] TAP detectado em <20ms após toque
- [x] 5 minutos sem toque: zero falsos positivos
- [x] TAP vs LONG_PRESS: distinguíveis de forma confiável em 20 tentativas
- [x] Operação simultânea de servos + touch: sem interferência perceptível
- [x] Touch não entra em chatter ao ficar próximo do threshold
- [x] Baseline permanece estável em repouso por 10 minutos
- [x] Recalibração lenta compensa drift ambiental sem gerar falso evento
- [x] Durante toque contínuo, baseline não deriva de forma a “engolir” o toque
- [x] `WAKE` em estado `SLEEPING` funciona de forma confiável
- [x] Métricas de debug (raw, baseline, threshold, state) refletem corretamente o comportamento observado
- [x] Serviço roda sem bloquear render, LEDs ou loop principal

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
- `audio_service`: task "nb_audio_task" Core0 prio6. VAD multi-feature sobre janelas de 256 samples (16ms):
  - High-pass one-pole (fc ≈ 180Hz) remove rumble antes do VAD.
  - RMS, ZCR, e 6 ratios espectrais via `sound_analysis_service` (FFT 256pt).
  - Threshold adaptativo ao ruído de fundo (EMA do piso de ruído × multiplicador).
  - Detecção de motor: heurística de frequência dominante + low_ratio + ZCR baixo.
  - Score acumulativo de entrada (frames fortes somam mais que frames suaves).
  - Silêncio configurável via `NB_AUDIO_VAD_SILENCE_MS` (padrão 1000ms).
- Threshold ajustável via `audio_service_set_vad_threshold()`.
- Eventos: `NB_EVT_VOICE_ACTIVITY_START`, `NB_EVT_VOICE_ACTIVITY_END`.
- Gravação de diagnóstico: `audio_record_diagnostic(path, duration_s)` → WAV 16-bit mono no SD.
- **Nota arquitetural:** O VAD atual detecta atividade de voz mas não é o ativador de sessão LLM. Ver Etapas 12.3–12.4 para a arquitetura atual: contrato explícito de sessão, touch como interação e wake word como ativador de escuta.

**Critérios de aceitação:**

- [x] Gravação de 3s: PCM audível sem artefatos (verificar via playback)
- [x] Falar perto do mic: `VOICE_ACTIVITY_START` em <200ms
- [x] Silêncio por 1000ms: `VOICE_ACTIVITY_END` publicado
- [x] Moto/carro passando: VAD não dispara `VOICE_ACTIVITY_START` (ZCR ≈ 0, low_ratio alto)

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
- Assets esperados em `/sdcard/assets/audio/`: greet_01–03, wake_up, sleep_enter, timer_done, reminder_due, alarm_due, error_01. Estados visuais, touch e escuta não disparam áudio por padrão.
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

- [x] Todas as transições de estado loggadas com motivo e timestamp
- [x] Emotion decai para neutral após 60s: verificado
- [x] 9 emoções → 9 faces distinguíveis: verificado visualmente
- [x] Timeout IDLE → SLEEPING: configurável via NVS, funcionando

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

- [x] Observar robot em idle por 2 minutos: parece **vivo**, não mecânico

**Critérios mensuráveis:**

- [x] Intervalo de blinks: nenhum <1.7s, nenhum >18s em observação de 5min
      (recalibrado contra vídeo idle do EMO — ver `docs/IDLE_REFERENCE.md`).
      Critério antigo (<1.5s / >10s) era inconsistente com a cauda Poisson.
- [x] Micro-movements de pescoço: amplitude <5°, frequência ≤ 3/minuto (stub — pós 3.3)
- [x] Aversive gaze: olhar se desvia a cada 8-15s em modo ATTENTIVE
- [x] Em IDLE: motifs longos (curiosity / head-tilt / look-down-blink) a
      cada 15–40s. Não saccades laterais frequentes — o vídeo do EMO
      mostra motifs **sustentados** (3–5s) mais do que glances rápidos.

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

- [x] Touch → expressão: latência percebida <100ms
- [x] 15 blinks observados: nenhum idêntico em timing ao anterior
- [x] NEUTRAL → HAPPY em 300ms: suave, sem frame perceptível de pulo
- [x] Fila de expressões: 3 expressões enfileiradas, executadas em sequência

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

- [x] Ação GREET: observador externo percebe face, motion e áudio como **uma** expressão unificada, não três outputs separados
- [x] Ação SLEEP: transição gradual e suave (>2s de fade)
- [x] Interrupt: nova ação enquanto ação em curso → transição limpa, sem movimento brusco

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

- [x] FPS de render nunca abaixo de 25fps com áudio simultâneo
- [x] Latência touch → resposta visual <100ms consistentemente em 50 tentativas
- [x] Temperatura dos servos estável após 30min de uso normal
- [x] Zero panics em 1 hora de operação contínua
- [x] `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` ≥ 300KB ao final

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

- [x] Após 100 interações simuladas: dados persistidos corretamente no SD
- [x] `ltm_is_user_familiar()` muda de false para true após threshold de interações
- [x] Corrupção simulada de arquivo de memória: sistema re-inicializa arquivo, não para
- [x] Flush assíncrono: não bloqueia tasks de prioridade ≥ 10

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

> Objetivo: Ativar periféricos adiados e novos sensores sobre uma fundação I2C compartilhada.
> Todos os HALs de sensor dependem do `nb_i2c_hal` (8.0) — nunca inicializam I2C por conta própria.

---

### Etapa 8.0 — `nb_i2c_hal` — Barramento I2C Compartilhado

**Dependências:** Bloco 7 concluído
**Hardware necessário:** Não (só firmware)

**O que entra:**

- `nb_i2c_hal`: inicializa `i2c_master_bus_create()` uma única vez nos GPIO 4 (SDA) e 5 (SCL).
- `nb_i2c_hal_init()`: configura bus, pull-ups internos, clock 400kHz, retorna `esp_err_t`.
- `nb_i2c_hal_get_bus()`: retorna `i2c_master_bus_handle_t` para uso pelos HALs de sensor.
- `nb_i2c_hal_scan()`: detecta endereços ativos no barramento — usado em boot debug e diagnósticos.
- Sensores com pino INT (IMU, APDS-9960) usam polling via schedule_service — GPIO 3 está ocupado pelo WS2812 RMT.
- `nb_i2c_hal_deinit()`: desmonta bus de forma segura (usado em power-off / safe mode).

**Endereços esperados no barramento (referência):**

| Dispositivo | Endereço | Etapa |
|-------------|----------|-------|
| OV2640 (SCCB) | 0x3C | 8.1 |
| MPU-6050 IMU | 0x68 | 8.2 |
| SHT40 temp/humid | 0x44 | 8.3 |
| APDS-9960 proximidade | 0x39 | 8.4 |
| bq25185 charger | 0x6B | 8.5 |
| MAX17048 fuel gauge | 0x36 | 8.5 |

**Critérios de aceitação:**

- [ ] `nb_i2c_hal_init()` retorna `ESP_OK` no boot
- [ ] `nb_i2c_hal_scan()` lista os endereços dos dispositivos conectados sem erro
- [ ] Dois HALs usando `nb_i2c_hal_get_bus()` simultaneamente: sem colisão de barramento
- [ ] `nb_i2c_hal_deinit()` libera recursos sem leak

---

### Etapa 8.1 — Câmera OV2640 ✓ parcial

**Dependências:** 8.0 concluída, 300KB PSRAM headroom verificado
**Hardware necessário:** Sim (FPC câmera conectado — pinos já reservados)

**O que entra:**

- `camera_hal`: backend ESP-IDF `esp_video`/V4L2, DVP, sessão diagnóstica sob demanda.
- Usa `nb_i2c_hal_get_bus()` para comunicação SCCB — não inicializa I2C próprio.
- Buffers pertencem ao driver de vídeo via sessão V4L2; o serviço não pré-aloca framebuffer de display.
- API: `camera_hal_capture()`, `camera_hal_get_frame()`, `camera_hal_release_frame()`.
- Task de captura separada: Core 1, prio 4 (abaixo de safety e render).
- Modos iniciais expostos pela Companion API:
  - `safe`: captura conservadora para diagnóstico.
  - `better`: captura 640×480 usada pela visão/observação.

**Implementado / validado em hardware (2026-05-25):**

- Câmera OV2640 detectada via SCCB (`PID=0x26`).
- Backend migrado para `esp_video`/V4L2, inspirado no modelo de sessão do StackChan, sem copiar C++ para fora do display.
- Capturas 640×480 funcionando pelo dashboard externo e por endpoint HTTP.
- `/api/vision/observe` retorna observação estruturada com resolução, tamanho JPEG, tempo de captura, brilho, contraste e movimento.
- Sessão de câmera abre sob demanda e fecha por timeout para evitar pressão permanente de DMA/internal heap.
- Bridge conectado e TTS funcionando junto ao suporte de câmera.

**Critérios de aceitação:**

- [x] Frame capturado sem artefatos visuais graves: verificado via dashboard externo/endpoint
- [x] PSRAM após captura: ≥ 300KB livre
- [x] Bridge e TTS permanecem funcionais com câmera compilada e testada
- [ ] FPS de render mantido ≥ 30fps com câmera ativa por teste longo
- [ ] Zero interferência com áudio (I2S0) e display (SPI2) em teste de 30 minutos
- [ ] Captura sob demanda repetida por 30 minutos sem OOM, watchdog ou queda de bridge

---

### Etapa 8.2 — IMU MPU-6050

**Dependências:** 8.0 concluída
**Hardware necessário:** Sim (MPU-6050 conectado via I2C — GPIO 4/5)

**O que entra:**

- `imu_hal`: driver I2C para MPU-6050 (0x68). Leitura de acelerômetro + giroscópio a 50Hz.
- Usa `nb_i2c_hal_get_bus()` — sem inicialização I2C própria.
- Tap detection via interrupt no GPIO 3 (sem polling).
- `NB_EVT_IMU_TAP`: batida física detectada pelo acelerômetro.
- `NB_EVT_IMU_SHAKE`: agitação detectada (threshold em NVS).
- Detecção de pouso: robot colocado na mesa após ser carregado → greet.

**Critérios de aceitação:**

- [ ] Tap na mesa: `NB_EVT_IMU_TAP` publicado em <50ms
- [ ] Shake (sacudir o robot): `NB_EVT_IMU_SHAKE` publicado
- [ ] 5 minutos de leitura contínua: zero interferência com render e áudio
- [ ] I2C compartilhado com outros HALs: sem colisão de barramento

---

### Etapa 8.3 — Sensor de Temperatura e Humidade SHT40

**Dependências:** 8.0 concluída
**Hardware necessário:** Sim (SHT40 conectado via I2C — GPIO 4/5)

**O que entra:**

- `nb_env_hal`: driver I2C para SHT40 (0x44). Leitura a 0.5Hz via schedule_service.
- Usa `nb_i2c_hal_get_bus()` — sem task dedicada, sem polling manual.
- `NB_EVT_ENV_UPDATE`: payload com `float temp_c` e `float humidity_pct`.
- Behavior reage: temp > 35°C → expressão de desconforto; humid > 80% → comportamento letárgico.
- Valores persistidos em NVS para baseline de ambiente.

**Critérios de aceitação:**

- [ ] Leitura de temperatura dentro de ±0.5°C vs termômetro de referência
- [ ] `NB_EVT_ENV_UPDATE` publicado a cada 2s sem falhas em 10 minutos
- [ ] Behavior muda expressão ao simular temp alta via mock
- [ ] Zero interferência com outros HALs no barramento

---

### Etapa 8.4 — Sensor de Proximidade APDS-9960

**Dependências:** 8.0 concluída
**Hardware necessário:** Sim (APDS-9960 conectado via I2C — GPIO 4/5, INT → GPIO 3)

**O que entra:**

- `nb_prox_hal`: driver I2C para APDS-9960 (0x39). Modo proximidade + luz ambiente.
- Usa `nb_i2c_hal_get_bus()`. Polling a 10Hz via schedule_service — GPIO 3 ocupado pelo WS2812.
- `NB_EVT_PRESENCE_NEAR`: objeto/pessoa a menos de ~20cm.
- `NB_EVT_PRESENCE_FAR`: proximidade volta ao baseline.
- `NB_EVT_AMBIENT_LIGHT`: nível de luz ambiente (lux) atualizado a 1Hz.
- Behavior reage: SLEEPING → acorda ao detectar presença antes de qualquer touch.
- Gesture recognition reservado para etapa futura.

**Critérios de aceitação:**

- [ ] Mão a ~15cm: `NB_EVT_PRESENCE_NEAR` publicado em <200ms
- [ ] Mão removida: `NB_EVT_PRESENCE_FAR` publicado em <300ms
- [ ] Robot em SLEEPING acorda via proximidade sem touch físico
- [ ] Polling 10Hz: CPU overhead < 1% medido com task monitor
- [ ] Zero colisão com IMU no barramento I2C compartilhado

---

### Etapa 8.5 — Bateria e Gestão de Energia

**Dependências:** 8.0 concluída, hardware de bateria presente (nova versão de placa)
**Hardware necessário:** bq25185 (0x6B), MAX17048 (0x36), TPS61088 (boost 5V)

**O que entra:**

- `battery_hal`: driver I2C para bq25185 (charger, 0x6B) e MAX17048 (fuel gauge, 0x36).
- Usa `nb_i2c_hal_get_bus()` — sem inicialização I2C própria.
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

### Etapa 8.6 — Touch Zones MPR121

**Dependências:** 8.0 concluída
**Hardware necessário:** Sim (MPR121 conectado via I2C — GPIO 4/5, eletrodos de fita de cobre no corpo do robô)

**O que entra:**

- `nb_touch_zone_hal`: driver I2C para MPR121 (0x5A). Polling a 20Hz via schedule_service.
- Usa `nb_i2c_hal_get_bus()` — sem inicialização I2C própria.
- 12 canais independentes reportados como bitmask. Cada canal mapeado a uma zona do corpo.
- Eletrodos: fita de cobre embutida no enclosure, funciona através de plástico fino (≤ 3mm).
- Coexiste com o touch nativo ESP32-S3 (GPIO 2) — são sistemas independentes.

**Mapeamento de zonas (ELE0–ELE4 ativos, ELE5–11 reserva):**

| Eletrodo | Zona | Evento |
|----------|------|--------|
| ELE0 | Cabeça (topo) | `NB_EVT_TOUCH_ZONE_HEAD` |
| ELE1 | Lado esquerdo | `NB_EVT_TOUCH_ZONE_LEFT` |
| ELE2 | Lado direito | `NB_EVT_TOUCH_ZONE_RIGHT` |
| ELE3 | Costas | `NB_EVT_TOUCH_ZONE_BACK` |
| ELE4 | Barriga/frente | `NB_EVT_TOUCH_ZONE_BELLY` |
| ELE5–11 | — | Reserva para expansão |

**Comportamentos via behavior_engine:**

- `TOUCH_ZONE_HEAD` → HAPPY + `synth_purr()`
- `TOUCH_ZONE_LEFT` / `RIGHT` → CURIOUS + gaze vira para o lado tocado
- `TOUCH_ZONE_BACK` → SURPRISED (toque inesperado)
- `TOUCH_ZONE_BELLY` → HAPPY máximo + expressão satisfeita

**Critérios de aceitação:**

- [ ] Toque em cada zona: evento correto publicado em < 100ms
- [ ] Dois toques simultâneos em zonas diferentes: ambos os eventos publicados
- [ ] 10 minutos de polling a 20Hz: CPU overhead < 1% no task monitor
- [ ] Touch nativo GPIO 2 não interfere com MPR121 e vice-versa
- [ ] Eletrodo coberto por plástico 2mm: detecção mantida

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

### Etapa 12.1 — Protocolo Bridge (Layer 2) ✓

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
AUDIO_CHUNK   payload: pcm_raw int16[], 256 samples = 512 bytes (16ms @ 16kHz)
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
- [x] AUDIO_CHUNK stream via TCP: jitter < 10ms entre chunks em rede local
- [x] TCP cai durante conversa: sistema detecta em < 10s, retorna offline
- [x] CRC8 com erro: frame descartado, contabilizado, sem crash

---

### Etapa 12.2 — Pipeline LLM via Bridge ✓

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
- Whisper (local, small model) → transcrição com filtro de `no_speech_prob`.
- Gemini Flash free tier com system prompt de persona do NoiseBot.
- Response parser: extrai intenção + emoção + texto de resposta.
- `--dry-run`: transcreve com Whisper, loga resultado, não chama Gemini/Piper.
- Piper TTS → WAV → envia em chunks via `SAY`.
- Envia `EXPR` e `ACTION` conforme intenção da resposta.

**System prompt do NoiseBot (base):**

- Personalidade do robot conforme persona_service (warmth/trust/energy).
- Contexto de estado atual (estado, emoção, uptime, familiaridade).
- Respostas curtas (< 10s de fala), nunca explicativas — sempre expressivas.

**Nota:** O contrato de sessão desta etapa (VAD → VOICE_START → chunks → VOICE_END) é refatorado nas Etapas 12.3 e 12.4 para eliminar sessões vazias, ativações por ruído ambiente e escuta involuntária por touch.

**Critérios de aceitação:**

- [x] Bridge conecta via TCP, handshake OK
- [x] Pipeline completo: voz → Whisper → Gemini → Piper → speaker
- [x] `EXPR` e `ACTION` chegam: face e motion coordenados com a fala
- [x] Bridge offline: robot expressa confusão (CURIOUS) e retorna a idle
- [x] Pergunta simples ("tudo bem?"): resposta em < 8s após VOICE_END
- [x] 10 conversas consecutivas: zero crash, sem degradação de memória

---

### Etapa 12.3 — Contrato de Sessão de Escuta (Session Contract) ✓

**Dependências:** 12.2 concluída
**Hardware necessário:** Não
**Status:** Implementado e validado em hardware

**Contexto:** O VAD heurístico atual ativa `bridge_tx_active` diretamente, causando sessões vazias (VOICE_END sem áudio), falsas ativações por ruído e chamadas Gemini com texto garbage. Esta etapa estabelece o contrato correto sem mudar a arquitetura de ativação.

**O que entra:**

No `audio_service`:

- API explícita de sessão de escuta (substitui o gate direto via VAD):
  ```c
  typedef enum { NB_LISTEN_SOURCE_TOUCH, NB_LISTEN_SOURCE_WAKE_WORD, NB_LISTEN_SOURCE_DEBUG } nb_listen_source_t;
  typedef enum { NB_LISTEN_END_VAD_SILENCE, NB_LISTEN_END_TIMEOUT, NB_LISTEN_END_BRIDGE_DISCONNECTED, NB_LISTEN_END_CANCELLED } nb_listen_end_reason_t;

  esp_err_t audio_service_begin_listen_session(nb_listen_source_t source);
  esp_err_t audio_service_end_listen_session(nb_listen_end_reason_t reason);
  bool      audio_service_is_listening(void);
  ```
- Flags internas de sessão: `listen_session_active`, `bridge_start_sent`, `bridge_audio_sent`.
- Invariantes obrigatórios:
  - `NB_EVT_VOICE_ACTIVITY_END` só é enviado à bridge se `bridge_start_sent && bridge_audio_sent`.
  - Chunks de áudio só são enviados se `listen_session_active && bridge_tx_active`.
  - `bridge_audio_sent = true` quando ao menos um chunk é enviado com sucesso.
  - Se bridge offline: sessão existe (visual/comportamental), mas sem VOICE_START, sem chunks, sem VOICE_END.
- Timeout de sessão sem fala: 8s sem `bridge_audio_sent` → `end_listen_session(NB_LISTEN_END_TIMEOUT)`.
- VAD não activa `bridge_tx_active` em estado IDLE — apenas fecha sessão aberta (`NB_LISTEN_END_VAD_SILENCE`).

No `bridge.py`:

- `--dry-run`: transcreve com Whisper, loga, não chama Gemini/Piper.
- Rejeição pré-Whisper: buffer vazio, samples < 8000 (< 0.5s).
- Rejeição pós-Whisper: texto vazio, `no_speech_prob` alto, `avg_logprob` ruim, `compression_ratio` suspeito.
- Log de sessão no final de cada pipeline: duração, samples, RMS médio, texto, motivo de descarte.

**Arquivos modificados:** `audio_service.h`, `audio_service.c`, `bridge.py`

**Critérios de aceitação:**

- [x] Bridge ligada + dry-run + wake word + fala: VOICE_START → chunks → VOICE_END → transcrição logada, Gemini não chamado
- [x] Bridge desligada + wake word + fala: sessão existe visualmente, zero frames enviados, zero VOICE_END ← validado em serial (bridge_start=0, VOICE_END suprimido)
- [x] Wake word + silêncio de 8s: timeout fecha sessão, VOICE_END não enviado (bridge_audio_sent=false), Gemini não chamado ← invariante implementado
- [x] Monitor serial: nunca `NB_EVT_VOICE_ACTIVITY_END` sem `NB_EVT_VOICE_ACTIVITY_START` precedente na mesma sessão ← garantido pelo contrato bridge_start_sent && bridge_audio_sent
- [x] Bridge.py: nunca log de chamada Gemini com text="" ou < 2 palavras reais

---

### Etapa 12.4 — Touch como Interação, não Escuta ✓

**Dependências:** 12.3 concluída, 2.2 (touch_service) concluída
**Hardware necessário:** Sensor de toque conectado
**Status:** Implementado e validado em hardware

**Contexto:** O protótipo inicial usou touch como ativador de sessão, mas o robô tem apenas um canal de toque e ele precisa servir como interação afetiva contínua. Manter touch abrindo escuta fazia qualquer carinho/tap virar captura de áudio, consumindo memória e poluindo o fluxo conversacional. A decisão final desta etapa é: **touch reage emocionalmente; escuta abre por wake word**.

**O que entra:**

Na `state_machine`:

- `state_machine_on_touch_tap()` em IDLE/SLEEPING: transita para `NB_STATE_TOUCH_REACTING`.
- Em `NB_STATE_ATTENTIVE`: timer de 8s de inatividade → `ATTENTIVE → IDLE` se `audio_service_is_listening() == false`.
- `state_machine_on_voice_end()`: `ATTENTIVE → RESPONDING` (se SAY for esperado) ou `ATTENTIVE → IDLE`.

No `boot_manager`:

- Handler de `NB_EVT_STATE_CHANGED` para ATTENTIVE: abre sessão somente quando a transição veio de `NB_EVT_WAKE_WORD_DETECTED`.
- Touch continua alimentando emotion model, conductor e LTM, sem abrir bridge nem capturar áudio.

No `audio_service`:

- `begin_listen_session(NB_LISTEN_SOURCE_TOUCH)`: bloqueado com `ESP_ERR_NOT_SUPPORTED`.
- `begin_listen_session(NB_LISTEN_SOURCE_WAKE_WORD)`: seta `listen_session_active = true`, reseta flags, aguarda fala real, envia `NB_EVT_VOICE_ACTIVITY_START` somente quando áudio começa a fluir.
- Em IDLE: VAD continua rodando para `sound_analysis` e comportamento semântico, mas nunca seta `bridge_tx_active` nem chama `begin_listen_session()`.
- VAD fecha sessão via `end_listen_session(NB_LISTEN_END_VAD_SILENCE)` após `NB_AUDIO_VAD_SILENCE_MS` de silêncio dentro de sessão ativa.

**Arquivos modificados:** `state_machine.c`, `boot_manager.c`, `audio_service.c`

**Critérios de aceitação:**

- [x] Moto/carro/TV sem wake word: zero `VOICE_START` na bridge, zero chunks, zero Gemini
- [x] Toque em IDLE: estado → `TOUCH_REACTING`, sem `[ PODE FALAR ]`
- [x] Toque em SLEEPING: estado → `TOUCH_REACTING`, retorna a `IDLE`, sem sessão de voz
- [x] Touch livre repetido: não abre bridge, não envia áudio, não cria `VOICE_END`
- [x] Tentativa legada de `NB_LISTEN_SOURCE_TOUCH`: retorna `ESP_ERR_NOT_SUPPORTED`
- [x] Wake word continua sendo o único ativador normal de escuta conversacional

---

### Etapa 12.5 — Pre-roll Ring Buffer ✓

**Dependências:** 12.4 concluída; 12.6 validada para wake word
**Hardware necessário:** Não

**Contexto:** Quando o usuário fala logo após a wake word, os primeiros 100–200ms da fala útil podem ser perdidos porque o streaming começa após a sessão ser aberta. Um ring buffer circular resolve isso sem alterar o contrato de sessão.

**O que entra:**

No `audio_service`:

- Ring buffer estático de 20 chunks × 256 samples × 2 bytes = **10 KB SRAM**.
  - Aloca em SRAM (não PSRAM) — DMA e acesso frequente requerem latência baixa.
  - Buffer circular: sobrescreve o mais antigo quando cheio.
  - Alimentado a cada ciclo da audio_task, independente do estado da sessão.
- `begin_listen_session()`: faz flush do ring buffer para bridge quando o streaming real começa.
  - Flush conta para `bridge_audio_sent` se ao menos um chunk for enviado com sucesso.
  - Flush é feito com timestamps retroativos para Whisper ter o áudio completo.
- Pre-roll de ~320ms cobre a transição wake word → fala útil + primeira sílaba.

**Arquivos modificados:** `audio_service.c`, `audio_service.h` (documentação)

**Critérios de aceitação:**

- [x] Wake word + fala imediata "que horas são": Whisper recebe áudio com primeira palavra preservada
- [x] Ring buffer não interfere com playback: `vad_playback_mute_ms` ainda inibe VAD durante TTS
- [x] Memória: `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` não cai abaixo de 50 KB após init
- [x] Flush do pre-roll precede chunks novos em ordem temporal correta na bridge

---

### Etapa 12.6 — Wake Word via ESP-SR (Investigação e Integração) ✓

**Dependências:** 12.4 concluída (touch como interação), 12.5 concluída
**Hardware necessário:** INMP441 já conectado

**Contexto:** Esta etapa estabelece uma wake word local como ativador primário de conversa. O toque permanece como canal de interação afetiva, não como fallback de escuta. A estratégia é usar primeiro um modelo pronto do ESP-SR (sem treinamento), validar memória e integração, e só depois considerar wake word customizada.

**O que entra:**

Investigação primeiro (não implementar sem validar):

- Adicionar `espressif/esp-sr: ">=1.6.0"` em `idf_component.yml`.
- Medir PSRAM livre após AFE init: deve manter > 300 KB de headroom.
- Medir tamanho de firmware: verificar se partição OTA suporta.

Se investigação for viável, implementar:

- `wake_service` (Layer 4): wrapper sobre AFE + WakeNet.
  - Wake word inicial: **"Hi ESP"** (WakeNet9, modelo pronto, sem treinamento).
  - Pipeline: `audio_hal` → `afe_handle->feed(pcm)` → WakeNet → `NB_EVT_WAKE_WORD_DETECTED`.
  - AEC (cancelamento de eco): fase posterior — requer loopback do speaker.
  - Task dedicada (Core 0, prio 7, 4KB).
- `nb_events.h`: adicionar `NB_EVT_WAKE_WORD_DETECTED`.
- `boot_manager`: handler `NB_EVT_WAKE_WORD_DETECTED` → `audio_service_begin_listen_session(NB_LISTEN_SOURCE_WAKE_WORD)`.
- Feedback ao acordar: expressão + LED reagem antes de começar captura.
- NVS: `nb_svc/ww_enabled` — desabilita WakeNet sem reflash.

Wake word customizada ("Oi NoiseBot", "Hey NoiseBot"):

- Requer ESP-SR Customization Tool ou fine-tuning.
- Fase separada, posterior a esta etapa — não bloquear integração por isso.
- "Noise" sozinho é curto demais e ambíguo — não usar como única keyword.

**Arquivos criados/modificados:** `components/services/wake_service/` (novo), `nb_events.h`, `boot_manager.c`, `idf_component.yml`, `sdkconfig.defaults`

**Critérios de aceitação:**

- [x] Build com esp-sr: zero warnings novos, zero regressão de funcionalidade
- [x] PSRAM livre após AFE init: > 300 KB — **7478 KB livres** (headroom câmera OK)
- [x] Dizer "Hi ESP": `NB_EVT_WAKE_WORD_DETECTED` no log, estado → ATTENTIVE, bot escuta
- [x] Ruído ambiente sem keyword: zero false positives em 5 minutos
- [x] Toque continua funcionando como interação independente do `ww_enabled`, sem abrir escuta
- [x] `ww_enabled=0` em NVS: WakeNet desabilitado, touch continua funcionando como interação

---

### Etapa 12.7 — Bridge Dry-Run e Transporte Confiável ✓

**Dependências:** 12.3, 12.5 e 12.6 concluídas
**Hardware necessário:** ESP32-S3 + PC/RPi executando `bridge.py --dry-run`

**Contexto:** Os testes pós-12.6 mostraram sessões em que a bridge recebia `VOICE_START`, mas encerrava com `samples=0`. Antes de mexer no VAD ou no wake word, o contrato físico de transporte precisa ficar provado: `VOICE_START -> AUDIO_CHUNK(s) -> VOICE_END`, com áudio real chegando ao bridge.

**O que entra:**

No `bridge_service`:

- Garantir envio completo de frames TCP: `send()` deve repetir até transmitir todo o frame ou detectar erro.
- Em queda de socket: publicar estado offline, limpar fila TX e permitir reconexão limpa.
- `bridge_service_flush_tx()` antes de uma nova sessão: remove resíduos de sessão anterior.
- Logs de diagnóstico compactos para:
  - `VOICE_START` enfileirado/enviado.
  - primeiro `AUDIO_CHUNK` enviado.
  - `VOICE_END` enviado ou suprimido.
  - desconexão/reconexão.

No `audio_service`:

- `begin_listen_session()` só marca `bridge_tx_active = true` se `VOICE_START` entrou na fila da bridge com sucesso.
- `bridge_audio_sent` só vira `true` após ao menos um chunk aceito pelo `bridge_service`.
- Pre-roll conta como áudio apenas se o chunk entrou na fila com sucesso.
- Se bridge não estiver conectada, a sessão continua visualmente, mas não tenta enviar frames.

No `bridge.py`:

- Detectar socket fechado (`recv() == b""`) como desconexão, não como ausência silenciosa de dados.
- Logar o primeiro chunk de áudio por sessão.
- Em `--dry-run`, nunca chamar Gemini/Piper; o sucesso do teste é `samples > 0` e transcrição/log de descarte coerente.

**Arquivos prováveis:** `bridge_service.c`, `bridge_service.h`, `audio_service.c`, `bridge.py`

**Critérios de aceitação:**

- [x] Bridge ligada + dry-run + wake word: terminal mostra `VOICE_START recebido`.
- [x] Bridge ligada + dry-run + wake word + fala: terminal mostra primeiro `AUDIO_CHUNK` e `samples > 0`.
- [x] `VOICE_START` sem nenhum chunk por 8s: bridge descarta como `buffer_vazio` sem chamar Gemini/Piper.
- [x] 5 sessões de wake word consecutivas: todas aparecem no terminal da bridge.
- [x] Desconectar/reconectar bridge: firmware volta a enviar sessão sem reboot.

---

### Etapa 12.8 — WakeNet Single-Shot e Rearm Seguro ✓

**Dependências:** 12.6 concluída, 12.7 validada com wake word
**Hardware necessário:** INMP441 + modelo WakeNet pronto ("Hi ESP")

**Contexto:** Wake word deve ser um gatilho de intenção, não um detector contínuo que continua disparando durante `ATTENTIVE`. Os logs mostraram múltiplas detecções de wake em sequência e warnings do AFE quando o fetch rodava sem áudio disponível. A referência XiaoZhi usa AFE/WakeNet como modo de IDLE, para o wake ao detectar, e só rearma quando o estado volta a permitir escuta de wake.

**O que entra:**

No `wake_service`:

- Avaliar e usar `fetch_with_delay(..., portMAX_DELAY)` se disponível na versão do ESP-SR.
- WakeNet é **single-shot** por sessão:
  - detectou wake -> suspende WakeNet imediatamente;
  - publica `NB_EVT_WAKE_WORD_DETECTED` uma única vez;
  - ignora novas detecções até rearm explícito.
- `wake_service_rearm()`:
  - só habilita WakeNet quando o sistema está de volta a `IDLE`/modo permitido;
  - reseta buffer AFE e buffer de feed;
  - aplica guard/cooldown para evitar wake residual.
- Medição de escala do PCM:
  - log de RMS/pico antes do `WAKE_INPUT_GAIN`;
  - log de saturação após ganho;
  - validar se `WAKE_INPUT_GAIN=16` ajuda ou satura.

No `boot_manager`/state flow:

- Wake word não deve ser rearmada por timeout parcial enquanto a sessão ainda está ativa.
- Durante `ATTENTIVE`/`RESPONDING`, WakeNet fica suspenso, exceto se uma etapa futura implementar interrupção explícita.

**Arquivos prováveis:** `wake_service.c`, `wake_service.h`, `boot_manager.c`, `state_machine.c`

**Critérios de aceitação:**

- [x] Dizer "Hi ESP" gera exatamente um `NB_EVT_WAKE_WORD_DETECTED` por sessão.
- [x] Durante `ATTENTIVE`, repetir "Hi ESP" não gera novo wake.
- [x] Após voltar para `IDLE`, "Hi ESP" volta a funcionar.
- [x] Zero spam de `AFE: Ringbuffer of AFE is empty` em operação normal.
- [x] Log de ganho mostra pico sem saturação persistente.

**Validação pós-12.9 (2026-04-21):**

- [x] Wake word recuperada após regressão de sensibilidade: `wn9_hiesp` com threshold final `0.55` e feed com PCM dedicado ao WakeNet.
- [x] `Hi ESP` transiciona `IDLE → ATTENTIVE`, abre sessão de fala e rearma WakeNet ao retornar para `IDLE`.
- [x] Caminho touch/bridge preservado usando buffer condicionado separado do buffer de WakeNet.

**Validação pós-remoção do touch-to-listen (2026-04-22):**

- [x] WakeNet opera no estilo StackChan/XiaoZhi: detectou wake word válida, suspende WakeNet e publica evento imediatamente.
- [x] Threshold ajustado para `0.55` em hardware, mantendo chamada real e reduzindo falsos positivos.
- [x] Filtro mínimo de energia rejeita disparos quase mudos sem bloquear chamadas reais de baixa energia.
- [x] `wake_service_rearm()` tornou-se idempotente: touch não deve resetar/logar rearme quando WakeNet já está armado.
- [x] Touch não abre mais escuta; `IDLE/SLEEPING → TOUCH_REACTING → IDLE` sem `[ PODE FALAR ]`.

---

### Etapa 12.9 — Turn-Taking Natural de Voz ✓

**Dependências:** 12.7 validada, 12.8 sem repetição de wake
**Hardware necessário:** INMP441 + bridge em dry-run

**Contexto:** Produtos conversacionais como StackChan/XiaoZhi não limitam a fala do usuário a uma janela curta fixa. A sessão deve ter dois tempos diferentes: um timeout curto para o usuário **começar** a falar e um limite longo de segurança enquanto ele **continua** falando. O fim normal do turno é silêncio pós-fala, não "acabou a janela".

**O que entra:**

No `audio_service`:

- Substituir a lógica conceitual de janela curta por estados internos de sessão:
  - `WAITING_FOR_SPEECH`: sessão aberta, aguardando primeira fala.
  - `CAPTURING_SPEECH`: fala detectada, áudio fluindo para bridge.
  - `ENDING_ON_SILENCE`: silêncio pós-fala acumulando para encerrar.
- Separar constantes:
  ```c
  LISTEN_WAIT_SPEECH_TIMEOUT_MS  /* ex.: 8000ms */
  LISTEN_END_SILENCE_MS          /* ex.: 1400ms, com graça inicial maior */
  LISTEN_MAX_SPEECH_MS           /* ex.: 30000-60000ms, safety */
  ```
- O timeout curto só roda em `WAITING_FOR_SPEECH`.
- Depois que a fala começa, a sessão não deve encerrar por `LISTEN_NO_VOICE_FALLBACK_MS`.
- Encerramento normal:
  - fala detectada -> silêncio contínuo pós-fala -> `VOICE_END`.
- Encerramento de segurança:
  - fala ou ruído contínuo por tempo máximo -> `VOICE_END` com motivo de safety.

No `bridge.py`:

- Aceitar sessões mais longas em dry-run sem truncar automaticamente.
- Logar duração, samples e motivo (`silence`, `wait_timeout`, `max_speech_timeout`, `buffer_vazio`).

**Arquivos prováveis:** `audio_service.c`, `audio_service.h`, `bridge.py`

**Critérios de aceitação:**

- [x] Wake word + silêncio: cancela após ~8s sem enviar Gemini/Piper.
- [x] Wake word + frase curta: encerra após parar de falar.
- [x] Wake word + fala por 15-20s: não corta antes do silêncio final.
- [x] Wake word + ruído contínuo: encerra por timeout máximo de segurança, sem travar.
- [x] Bridge dry-run mostra sessão longa com `samples > 0`.

---

### Etapa 12.10 — ESP-SR VAD para Listening ✓

**Dependências:** 12.7 e 12.9 validadas com wake word; 12.8 estável para wake
**Hardware necessário:** INMP441 mono; PSRAM habilitada
**Status:** Implementado e validado em hardware

**Contexto:** O VAD heurístico de RMS/ZCR/FFT é útil para consciência sonora, mas não deve ser o juiz principal da sessão LLM em ambiente real com TV, carros e motos. A referência XiaoZhi usa ESP-SR para wake/listening. Esta etapa migra o listening para VAD da ESP-SR, mantendo touch fora do fluxo de escuta.

**O que entra:**

Investigar primeiro:

- Verificar modelos disponíveis na partição `model`:
  - WakeNet (`ESP_WN_PREFIX`)
  - VADNet (`ESP_VADN_PREFIX`)
  - NSNet (`ESP_NSNET_PREFIX`)
- Medir PSRAM antes/depois de criar o AFE de voice processing.
- Confirmar se dois AFE handles simultâneos (WakeNet + Voice Processor) cabem na memória ou se será necessário alternar instâncias.

Implementação:

- Integração no `audio_service` usando `vad_create_with_param()`/`vad_process_with_trigger()`.
- Processar frames de 10ms / 160 samples em paralelo ao fluxo atual de áudio.
- Usar estado do ESP-SR VAD como decisão primária em `WAITING_FOR_SPEECH`/`CAPTURING_SPEECH`/silêncio.
- Manter heurística atual como fallback se o ESP-SR VAD não inicializar.
- Avaliar envio de `res->data` processado para bridge no lugar do PCM bruto.
- Preservar pre-roll/vad-cache para não cortar a primeira palavra.

**Arquivos modificados:** `audio_service.c`, `audio_service.h`, `components/services/audio_service/CMakeLists.txt`

**Critérios de aceitação:**

- [x] Boot loga `audio_svc: inicializado (... esp_vad=1)`.
- [x] Wake word + fala: ESP-SR VAD detecta fala e silêncio.
- [x] Wake word + frase longa: sessão fecha por silêncio pós-fala.
- [x] Moto/carro/TV sem wake word: zero sessão bridge.
- [x] Wake word + ruído sem voz: bridge descarta ou sessão encerra sem Gemini/Piper.
- [x] PSRAM livre após AFE listening: > 300 KB, ou decisão explícita de trade-off se câmera continuar adiada.

**Validação complementar de voz (2026-05-27):**

- [x] O pipeline RAW deixou de usar `vad_process_with_trigger()` como decisão
  contínua de silêncio e passou a usar `vad_process()`, alinhando o
  comportamento ao uso esperado do ESP-SR VAD.
- [x] Testes pós-flash confirmaram turnos curtos encerrando por
  `voice_end_reason=silence`, sem `audio_longo`.
- [x] O server passou a proteger a resposta da LLM contra saída em scripts
  chinês/japonês/coreano antes do TTS, mantendo resposta em português.
- [x] Timeout de `LISTENING` após barge-in deixou de virar `SESSION_ERROR`:
  ausência de nova fala agora encerra o turno como `listen_timeout` e cancela
  follow-up de forma limpa.
- [x] `voice_controller` iniciado como ponto unico para wake word, follow-up e
  efeitos principais de `ATTENTIVE`/`RESPONDING`, reduzindo politica
  conversacional espalhada no `boot_manager`.
- [x] Wake durante `RESPONDING` passou a abrir escuta como
  `NB_LISTEN_SOURCE_BARGE_IN`, permitindo diferenciar barge-in de wake normal
  nos logs e no tuning de turn-taking.
- [x] Barge-in passou a suprimir pre-roll antigo e limpar a fila de SAY no stop,
  evitando que a fala do proprio robo entre no começo do novo turno sem AEC.
- [x] Server abriu folga de captura para 192000 samples (12 s), evitando que
  fala válida em torno de 10 s seja descartada por alinhamento de chunk
  (`160256/160000`).
- [x] Contrato ficou alinhado ponta a ponta: fallback interno do server,
  `HELLO` do server, `HELLO` do firmware e documentação deixam de anunciar
  `160000` como teto efetivo do pipeline.
- [x] Barge-in por wake word durante resposta foi validado em hardware com
  harness `barge-live`: `outcome=interrupted`,
  `discard_reason=barge_in` e cancelamento em 0,9 ms.
- [x] No-echo foi validado com harness `no-echo-live`: após resposta longa não
  surgiu turno fantasma dentro de janela de 10 s.
- [x] Opus 16 kHz mono/60 ms foi validado como modo experimental opt-in em
  turno real e multi-turn, com STT `good`, LLM/local intent e zero drops.
- [x] AEC de dispositivo foi classificado como não promovível no hardware atual:
  `aec_blocked_no_reference=true`, `aec_supported=false`,
  `ESP_ERR_NOT_SUPPORTED`.
- [x] Opus foi promovido de experimento manual para capability oficial opt-in:
  `/ai/status` expõe `audio`, `codecs`, `features` e `firmware.*`, a Ops API
  proxy os endpoints Opus do firmware e PCM16 segue como fallback padrão.
- [x] Harness pareado `noisebot_server debug codec-ab` criado para comparar
  PCM16 vs Opus em frases repetíveis antes de qualquer promoção para padrão.
- [x] A/B curto PCM16 vs Opus executado em hardware: 3/3 turnos Opus `ok`,
  zero drops e 160 pacotes drenados; manter Opus como opt-in porque duas
  transcricoes Opus ficaram semanticamente piores que PCM16.
- [x] A/B maior PCM16 vs Opus executado em hardware: 10/10 turnos Opus
  `ok`, zero drops, 897 pacotes drenados e STT medio equivalente; manter Opus
  opt-in porque o match semantico estimado ficou abaixo de PCM16.
- [x] Diagnóstico offline sobre WAVs reais escolheu o próximo perfil live:
  Opus 16 kHz mono, frame de 60 ms e 32 kbps fixo. Isso mantém o contrato
  Xiaozhi/StackChan de frame/codec, mas privilegia robustez no ambiente real
  do NoiseBot antes de qualquer promoção para padrão.
- [x] Arquitetura `Voice Audio v2` documentada para refazer captura,
  reproducao, sessao, codec e processamento de voz de forma paralela,
  preservando PCM16/wake/barge-in atuais. Ver
  `docs/VOICE_AUDIO_V2_ARCHITECTURE.md`.
- [x] Migracao Opus v2 fechada como default local do server:
  `NOISEBOT_AUDIO_DEFAULT_CODEC=opus-v2`, worker live do
  `audio_codec_service_v2`, `codec-v2 health` limpo, zero drops, fila egress
  zero, `opus_codec_error=0`, barge/no-echo em Opus validados e rollback PCM16
  preservado por env/restart ou `codec-v2 transport-disable`.
- [x] Corte percebido de resposta foi separado de audio: TTS/playback completou
  e o problema observado era visual no `TEXT_SCROLL`; o server pagina texto por
  limite UTF-8 e largura visual aproximada sem novo opcode.
- [x] Proximas fases pos-Opus documentadas em
  `docs/VOICE_AUDIO_V2_NEXT_PHASES.md`: playback v2 como dono gradual do
  downlink, checklist/health de release, Voice Activity v2 shadow/opt-in,
  Capture Session v2 por flag e policy conversacional avancada. Sem mexer em
  wake threshold, AEC device-side, follow-up automatico ou barge-in sem wake
  junto com essas fases.
- [x] Fase I iniciada com observabilidade de downlink: Playback v2 agora
  observa a fila SAY real via notas do `audio_service` e expõe contadores
  `say_*` em `/api/audio/playback-v2`, sem assumir HAL, sem trocar a fila e sem
  alterar wake/captura/codec.
- [x] Fase I avancou para handoff parcial: `audio_playback_service_v2` assumiu
  a fila estatica SAY de 16 chunks e o `audio_service` ficou como consumidor
  seguro que drena para o HAL. `/api/audio/playback-v2` expoe
  `bridge_say_queue_owner=true`. Validado em hardware apos flash com 283
  chunks SAY recebidos/tocados, fila final zero, zero drops e `SAY_END`
  confirmado.
- [x] Regressao assistida de barge-in pos-handoff registrada em hardware:
  `/ai/metrics` mostrou `outcome=interrupted`, `discard_reason=barge_in` e
  cancelamento em 3,5 ms; Playback v2 terminou com fila SAY zero e `ESP_OK`.
- [x] Rodada controlada pos-restart confirmou a Fase I em hardware: server
  limpo, Opus v2 ativo, `ww -> que horas sao?` respondeu como `local_time` e
  `ww -> me conte uma historia longa -> ww -> pare` interrompeu a fala velha
  com `discard_reason=barge_in`, cancelamento p50 2,6 ms / p95 3,2 ms,
  Playback v2 com `say_queue_count=0`, `say_cancel_count=2`,
  `say_chunks_cancelled=28` e `ESP_OK`.
- [x] Stop curto pos-barge-in robustecido no server: em teste fisico, o STT
  confundiu `pare` com `Vale.`, mas o contexto de barge-in recente roteou para
  `local_stop` e respondeu `Pronto, parei.` sem LLM. O mesmo contrato cobre
  `Tchau.` como mishear apenas nessa janela; fora dela, despedida continua
  despedida normal. Playback v2 terminou com fila zero, Capture v2 desligado e
  `codec-v2 health` ok apos dreno de 1 pacote egress pendente.
- [x] Fase J iniciada localmente como shadow probe passivo: o
  `voice_activity_service_v2` recebe copia do PCM condicionado do
  `audio_service`, expoe `/api/audio/activity-v2` e shadow start/stop, mede
  RMS/peak/fala/silencio/mute/sessao ativa e nao altera wake, captura, bridge,
  codec, Playback v2 ou HAL. Validacao local: contrato focado Voice Audio v2 e
  build ESP-IDF limpos. Validacao em hardware apos flash: shadow de 1000 ms
  observou 63 frames, encerrou sozinho em 1008 ms, classificou silencio sem
  sessao ativa, `ESP_OK`; Playback v2 fila SAY zero, capture-v2 desligado e
  `codec-v2 health` ok apos reativar Opus v2.
- [x] Fase J ganhou telemetria ZCR passiva: `/api/audio/activity-v2` expoe
  `zcr_last_permille` e `zcr_max_permille` calculados sem float/no malloc. A
  metrica e apenas comparativa e nao muda wake, VAD, fim de fala, captura,
  playback, codec ou bridge. Validacao local: contrato focado e build ESP-IDF.
  Validacao em hardware apos flash: shadow de 1000 ms com 63 frames,
  `zcr_last_permille=98`, `zcr_max_permille=141`, silencio, sem sessao ativa e
  `codec-v2 health` ok apos reativar Opus v2.
- [x] Fase J ganhou contadores passivos de runs no shadow: Activity v2 expoe
  `speech_run_frames`, `silence_run_frames`, `speech_run_max_frames` e
  `silence_run_max_frames`. Esses campos preparam comparacao futura de
  VAD/end-of-speech, sem alterar wake, captura, Playback v2, codec, bridge ou
  HAL. Validacao local: contrato focado Voice Audio v2 e build ESP-IDF limpos.
  Validacao em hardware apos flash: shadow de 1000 ms observou 63 frames de
  silencio, com `silence_run_frames=63`, `silence_run_max_frames=63`,
  `speech_run_max_frames=0`, Playback v2 fila zero, Capture v2 desligado e
  `codec-v2 health` ok apos reativar Opus v2.
- [x] Fase J validada em turno real com shadow de 30 s: `ww -> me conte uma
  historia curta` gerou transcript bom, `tts_completed=true`,
  `tts_say_end_sent=true`, 253 chunks TTS e zero alerta. Activity v2 registrou
  `session_frames=384`, `muted_frames=334`, `speech_frames=45`,
  `speech_run_max_frames=7` e `silence_run_max_frames=521`. Playback v2
  terminou com fila zero e zero drops novos; Capture v2 desligado e Codec v2 ok.
- [x] Fase K iniciada de forma contratual: `/api/audio/capture-v2` agora expoe
  `end_reason`, `bridge_tx_owner=false` e
  `legacy_audio_service_tx_owner=true`, deixando explicito que o Capture v2
  ainda nao assumiu o bridge TX real. Validacao local: `server/tests`,
  `bridge/tests` e `idf.py build` limpos.
- [x] Fase K validada em hardware apos flash: replay diagnostico de Capture v2
  retornou `state=DONE`, `end_reason=SPEECH_COMPLETE`,
  `captured_samples=10240`, `dropped_frames=0`, `bridge_tx_owner=false` e
  `legacy_audio_service_tx_owner=true`. Opus v2 foi reativado e `codec-v2
  health` voltou `status=ok`; Playback v2 ficou com fila SAY zero.
- [x] Fase K avancou para shadow TX local: `/api/audio/capture-v2` agora expoe
  `shadow_voice_start_sent`, `shadow_voice_end_sent`, `shadow_audio_chunks`,
  `shadow_audio_samples` e `shadow_audio_dropped_chunks` para espelhar onde o
  Capture v2 emitiria `VOICE_START/AUDIO_CHUNK/VOICE_END`, mantendo
  `bridge_tx_owner=false` e o envio real no `audio_service`.
- [x] Shadow TX validado em hardware apos flash: replay diagnostico retornou
  `shadow_voice_start_sent=true`, `shadow_voice_end_sent=true`,
  `shadow_audio_chunks=40`, `shadow_audio_samples=10240`,
  `shadow_audio_dropped_chunks=0`, `captured_samples=10240`,
  `dropped_frames=0`, `bridge_tx_owner=false` e Codec v2 ok apos reativar
  Opus.
- [x] Turno real com Capture v2 ligado validou comportamento e revelou ajuste
  de unidade: `shadow_audio_chunks=158` acompanhou `chunk_count=158`, mas
  `shadow_audio_samples` somava chunks de 256 em vez de frames Opus de 960.
  Correcao local aplicada para contar `sent_packets *
  NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES`; build ESP-IDF e `bridge/tests` limpos.
- [x] Revalidacao em hardware apos flash confirmou samples alinhados:
  `shadow_audio_samples=52800` contra `total_samples=52784`, Playback v2 sem
  drops e Codec v2 ok. Ajuste local seguinte corrige tambem
  `shadow_audio_chunks`/`speech_elapsed_ms` para unidades Opus quando o shadow
  recebe pacotes de 960 samples.
- [x] Revalidacao final do shadow TX Opus: `shadow_audio_chunks=58` bateu com
  `chunk_count=58`, `shadow_audio_samples=55680` ficou alinhado com
  `total_samples=55664`, `speech_elapsed_ms=3480`, Playback v2 zero drops,
  Codec v2 ok e `voice_alert=null`. Capture v2 segue shadow/observador:
  `bridge_tx_owner=false`.
- [x] Gate local de handoff da Capture v2 adicionado ao status:
  `/api/audio/capture-v2` agora expoe `bridge_tx_candidate`,
  `bridge_tx_handoff_ready` e `handoff_block_reason`. O gate apenas classifica
  a sessao observada e nao transfere bridge TX; `audio_service` continua dono
  real de `VOICE_START/AUDIO_CHUNK/VOICE_END`. Validacao local: contrato
  focado, `bridge/tests`, `server/tests` e `idf.py build`.
- [x] Validacao em hardware do gate de handoff: replay diagnostico bloqueou
  corretamente por `NOT_REAL_CAPTURE`; turno real por wake ficou com
  `bridge_tx_candidate=true`, `bridge_tx_handoff_ready=true`,
  `handoff_block_reason=NONE`, zero drops, Playback v2 fila zero e Codec v2
  ok. Observacao para o proximo gate: esse turno curto encerrou no server por
  timeout (`voice_end_reason=timeout`, `duration_ms=9479`), entao repetir
  validacao curta por silencio antes de transferir TX real.
- [x] Repeticao curta por silencio do gate de handoff: `ww -> que horas sao`
  retornou transcript correto, `voice_end_reason=silence`, `duration_ms=3719`,
  `bridge_tx_handoff_ready=true`, `handoff_block_reason=NONE`,
  `shadow_audio_chunks=62`, `shadow_audio_samples=59520`, zero drops,
  Playback v2 com +404 chunks recebidos/tocados sem drops novos e Codec v2
  limpo apos drenar 1 pacote egress pendente.
- [x] Preparacao local do handoff real: adicionada flag NVS
  `voice_audio_v2_capture_tx_enabled` (`v2cap_tx_en`, default off), exposta em
  `/api/config`, `/api/config/all`, `/api/audio/capture-v2` como
  `bridge_tx_handoff_enabled` e no CLI `capture-v2 tx-enable|tx-disable`.
  Este passo nao altera o TX real; apenas cria o arm/disarm separado da flag
  de observacao.
- [x] Validacao pos-flash da flag de handoff: default desligado confirmado,
  `tx-enable`/`tx-disable` alternaram NVS, e turno real com a flag desligada
  manteve TX legado (`bridge_tx_owner=false`,
  `legacy_audio_service_tx_owner=true`) com gate verde, zero drops e
  `voice_end_reason=silence`. O health do Codec v2 voltou `status=ok` apos
  drenar 1 pacote egress pendente.
- [x] Reflash da correcao de infraestrutura HTTP e validacao de `/api/config/all`:
  havia 98 rotas para `max_uri_handlers=64`, entao APIs no fim da tabela podiam
  responder 404. O firmware agora dimensiona o limite a partir de `k_uris`.
  Pos-flash, `/api/config/all` respondeu JSON e confirmou
  `voice_audio_v2_capture_enabled=true` com
  `voice_audio_v2_capture_tx_enabled=false`.
- [x] Turno curto real apos reflash da correcao HTTP: `ww -> que horas sao`
  retornou transcript correto, `voice_end_reason=silence`, `duration_ms=2159`,
  `chunk_count=36`, `total_samples=34544`, TTS completo com `SAY_END`,
  `voice_alert=null`, Capture v2 com gate verde e TX legado, e Codec v2
  `status=ok`. Observacao: Playback v2 ainda carrega drops cumulativos de
  interacoes anteriores; acompanhar deltas antes do proximo handoff real.
- [x] Harness server de delta para Playback v2: novo CLI
  `noisebot_server --host 192.168.1.30 debug playback-v2 status|delta --json`
  consulta `/api/audio/playback-v2` e calcula deltas de
  `say_chunks_received/played/dropped/dropped_listening/cancelled` entre dois
  snapshots. Sanity check em hardware pos-reboot retornou contadores zerados,
  `queue_empty=true` e `normal_path_clean=true` sem turno entre snapshots.
- [x] Handoff real opt-in preparado localmente para Capture v2: com
  `voice_audio_v2_capture_enabled=true` e
  `voice_audio_v2_capture_tx_enabled=true`, o `audio_service` preserva o HAL e
  o condicionamento de audio, mas passa o envio logico de
  `VOICE_START/AUDIO_CHUNK/VOICE_END` para `voice_capture_session_v2`. Com a
  flag de TX desligada, o caminho legado segue ativo. Validacao local: contrato
  focado Voice Audio v2 e build ESP-IDF limpos; as validacoes fisicas
  seguintes aprovaram o handoff opt-in em hardware.
- [x] Validacao fisica do handoff real opt-in: apos flash, Opus reativado e
  `capture-v2 tx-enable`, um turno curto `ww -> que horas sao` fechou com
  `turn_id=47`, transcript correto, `voice_end_reason=silence`,
  `tts_completed=true`, `tts_say_end_sent=true`, `voice_alert=null` e Capture
  v2 como dono real do TX (`bridge_tx_owner=true`,
  `legacy_audio_service_tx_owner=false`). Capture v2 contou 104 chunks,
  99840 samples e zero drops; Playback v2 ficou com fila zero e zero drops; o
  unico pacote egress Opus pendente foi drenado e `codec-v2 health` voltou
  `status=ok`. A flag experimental foi desligada depois e `/api/config/all`
  confirmou `voice_audio_v2_capture_tx_enabled=false`.
- [x] Validacao fisica de barge-in com handoff real opt-in: com
  `capture-v2 tx-enable`, o roteiro
  `ww -> me conte uma historia longa -> ww -> pare` interrompeu a historia
  longa (`turn_id=49`, `outcome=interrupted`,
  `discard_reason=barge_in`) e reconheceu `Pare.` como `local_stop`
  (`turn_id=50`, resposta `Pronto, parei.`, TTS completo e `SAY_END`). Capture
  v2 ficou dono do TX no barge-in (`source=BARGE_IN`,
  `bridge_tx_owner=true`, `legacy_audio_service_tx_owner=false`), com
  67 chunks, 64320 samples e zero drops. Playback v2 encerrou com fila zero,
  1 cancelamento, 3 chunks cancelados e 7 drops apenas em
  `say_chunks_dropped_listening`, classificados como descarte correto de audio
  velho durante a nova escuta. Codec v2 permaneceu `status=ok`; a flag de TX
  foi desligada ao fim e `/api/config/all` confirmou
  `voice_audio_v2_capture_tx_enabled=false`.
- [x] Endurecimento local do rollback do handoff: ao desligar
  `voice_audio_v2_capture_tx_enabled`, o firmware tambem libera o ownership
  interno do Capture v2 em idle, evitando que `/api/audio/capture-v2` continue
  mostrando `bridge_tx_owner=true` apenas por causa da ultima sessao. Ativar
  ownership continua permitido somente durante sessao real ativa. Validacao
  local: contrato focado Voice Audio v2 e build ESP-IDF limpos. Apos flash,
  `capture-v2 tx-disable` confirmou rollback operacional com
  `bridge_tx_handoff_enabled=false`, `bridge_tx_owner=false` e
  `legacy_audio_service_tx_owner=true`.
- [x] Aceite final do Capture v2 TX opt-in em hardware: com Opus ativo e
  `capture-v2 tx-enable`, o turno curto `ww -> que horas sao` fechou em
  `turn_id=54` por silencio, com TTS completo, `SAY_END`, Capture v2 dono do
  TX real (`bridge_tx_owner=true`, `legacy_audio_service_tx_owner=false`), 78
  chunks / 74880 samples e zero drops. Playback v2 recebeu/tocou 401 chunks
  SAY com fila final zero e zero drops; Codec v2 ficou
  `healthy=true/status=ok`.
- [x] Revalidacao final de barge-in com handoff real opt-in: o turno da
  historia (`turn_id=57`) foi interrompido por `barge_in`; Capture v2 reportou
  `source=BARGE_IN`, `bridge_tx_owner=true`,
  `legacy_audio_service_tx_owner=false`, 113 chunks / 108480 samples e zero
  drops. Playback v2 encerrou com fila zero, 2 cancelamentos, 12 chunks
  cancelados e 6 drops apenas em `say_chunks_dropped_listening`, isto e,
  descarte esperado de audio velho durante a nova escuta. O comando final desta
  repeticao caiu como `local_farewell` em vez de `local_stop`, registrado como
  detalhe de policy/STT fora do escopo estrutural da Fase K. Codec v2
  permaneceu `healthy=true/status=ok`, e o rollback final por
  `capture-v2 tx-disable` voltou a `bridge_tx_owner=false`.
- [x] Fase L iniciada no server: a classificacao local agora trata variantes
  curtas de despedida/confirmacao apos barge-in recente como comando de
  interrupcao (`local_stop`), incluindo a transcricao real `Tchup! Bye!`.
  Fora da janela de barge-in, `Tchau.`/`Bye.` seguem como despedida normal.
  Isso nao altera wake, VAD, AEC, Capture v2, Playback v2, Codec v2, bridge nem
  firmware. Validacao local: intents focados e `server/tests` completos.
- [x] Fase L ampliou comandos diretos de cancelamento: `corta`, `corta isso`,
  `para de falar`, `chega disso`, `nao quero mais` e `encerra` agora viram
  `local_stop` mesmo fora de barge-in, junto de `pare`/`cancela`. Validacao
  local: intents focados e `server/tests` completos.
- [x] Fase L ganhou observabilidade de turn-taking: `/ai/metrics` agora
  registra `recent_barge_in` e `turn_taking_policy` (`normal` ou
  `post_barge_in`) em `last_voice_session`. A telemetria tambem inclui
  `turn_taking_decision` (`direct_stop`, `post_barge_stop`, `local_intent` ou
  `llm`), tornando auditavel por que um comando curto foi roteado como stop
  contextual. `voice_diagnosis` agora traduz stops diretos/contextuais em
  diagnostico operacional sem alerta falso. Validacao local: testes focados de
  metricas/orquestrador/diagnostico e `server/tests` completos.

---

### Etapa 12.11 — Rebaixar VAD Heurístico para Diagnóstico ✓

**Dependências:** 12.10 validada
**Hardware necessário:** Não
**Status:** Implementado e validado em hardware

**Contexto:** Depois que o AFE/VADNet governa o listening, o VAD heurístico atual deixa de ser caminho crítico para conversa. Ele continua valioso para comportamento emergente, sound analysis, detecção de eventos ambientais e calibração, mas não deve abrir/fechar sessão LLM.

**O que entra:**

- Documentar explicitamente no `audio_service.h`:
  - VAD heurístico não ativa bridge;
  - VAD heurístico não é fonte primária de `VOICE_END` da sessão LLM quando AFE/VADNet está ativo.
- Manter features atuais para:
  - `sound_analysis_service`;
  - `vad_semantic_service`;
  - eventos de comportamento (`VOICE_SOFT`, `VOICE_LOUD`, etc.);
  - logs/calibração.
- Remover ou isolar defines frágeis que só existiam para impedir falso positivo de bridge.
- Atualizar docs de arquitetura de voz.

**Arquivos modificados:** `audio_service.c`, `audio_service.h`, `boot_manager.c`, `docs/ARCHITECTURE.md`, `docs/ROADMAP.md`

**Critérios de aceitação:**

- [x] Desabilitar VAD heurístico não quebra sessão LLM com wake word.
- [x] Sound analysis continua classificando ambiente para comportamento.
- [x] Nenhum caminho em IDLE consegue enviar áudio para bridge sem wake word.
- [x] Documentação deixa claro qual VAD serve a qual finalidade.

---

### Etapa 12.12 — Avaliação Dual Mic, BSS e AEC (adiada)

**Dependências:** 12.10 validada com 1 mic; 12.11 concluída
**Hardware necessário:** Protótipo com segundo microfone ou codec adequado
**Status:** Adiada por decisão de produto — não desenvolver até haver hardware/necessidade real. O firmware agora declara essa limitação via `nb_board_caps_t` e bloqueia AEC de dispositivo quando a placa não tem referência limpa de playback.

**Contexto:** StackChan/CoreS3 usa dual microphones e codec ES7210, mas dois microfones não corrigem bugs de sessão, bridge, wake rearm ou timeouts. A avaliação dual mic só deve acontecer depois que 1 mic + AFE/VADNet + bridge estiver estável.

**O que entra:**

Investigação:

- Mapear opções de hardware:
  - segundo INMP441 em canal oposto do I2S;
  - codec multi-mic externo;
  - manter hardware atual e não avançar.
- Verificar conflito de GPIOs com câmera/IMU reservados.
- Definir espaçamento físico entre mics e orientação na carcaça.
- Avaliar `input_format` do AFE:
  - `"M"`: mono atual;
  - `"MM"`: dois microfones;
  - `"MR"`/`"MMR"`: microfone(s) + referência de playback se houver caminho limpo;
  - BSS/MISO quando suportado.
- AEC:
  - só habilitar se houver referência digital limpa do speaker;
  - não misturar com mute heurístico sem medição.
- Implementação base no firmware:
  - `nb_board_caps_t` declara `audio_input_reference=false` e
    `supports_device_aec=false` para o hardware INMP441 + MAX98357A atual;
  - `CONFIG_NB_USE_DEVICE_AEC` depende de
    `CONFIG_NB_BOARD_HAS_AUDIO_REFERENCE`;
  - `/api/audio/processor/aec/probe` retorna bloqueio explícito quando não há
    referência, em vez de tentar criar AFE `MR` como se a placa fosse CoreS3.

**Critérios de decisão:**

- Só migrar para dual mic se 1 mic falhar em ruído real mesmo com AFE/VADNet.
- Dual mic não deve consumir GPIO reservado da câmera.
- PSRAM livre deve continuar dentro do orçamento.

**Critérios de aceitação para eventual protótipo:**

- [ ] 1 mic vs 2 mics medidos com os mesmos cenários: silêncio, fala próxima, TV, carro/moto.
- [ ] BSS/AEC melhora transcrição ou reduz falso fim de fala de forma mensurável.
- [ ] Sem regressão de wake word.
- [ ] Sem queda de FPS/render ou instabilidade de áudio.

---

### Sub-bloco 12B — Bridge Agent Runtime e Comandos Locais

**Objetivo:** Evoluir o `bridge.py` de protótipo funcional para um serviço de produto robusto, observável e extensível, seguindo a arquitetura validada em produtos como StackChan/XiaoZhi: wake local, ASR, roteamento de intenção, comandos locais para tarefas simples, LLM para diálogo complexo e TTS para resposta expressiva.

**Princípios de produto:**

- Comandos simples não devem depender de Gemini/OpenAI.
- Falhas de LLM, STT ou TTS não podem parecer silêncio inexplicável.
- Toda sessão deve terminar com motivo explícito e reproduzível.
- O firmware atual deve continuar compatível durante a refatoração do bridge.
- O bridge deve permitir teste offline com áudio gravado, sem precisar acordar o robô a cada ajuste.
- A arquitetura deve preparar terreno para tools/MCP/Home Assistant sem acoplar essas integrações cedo demais.

**Referências arquiteturais adotadas:**

- Ver `docs/REFERENCE_ARCHITECTURES.md`.
- StackChan confirma a separação desejada: motor conversacional emite estados/tools; a camada do robô traduz isso em avatar, movimento, LED, toque e produto.
- XiaoZhi confirma o contrato de conversa desejado: wake local, listening/speaking explícitos, AFE/VAD separado, canal de áudio sob demanda, Opus/WebSocket e MCP/tools.
- O NoiseBot não será refeito em cima desses firmwares. As ideias entram por etapas pequenas, com compatibilidade, critérios de aceite e respeito às camadas existentes.

---

### Etapa 12.13 — Bridge Runtime Profissional ✓

**Dependências:** 12.11 concluída; 12.12 explicitamente adiada
**Hardware necessário:** Não obrigatório para desenvolvimento inicial; robô necessário para validação final

**Contexto:** O `bridge.py` atual provou o pipeline, mas concentra transporte, sessão, STT, filtros, Gemini, Piper, dry-run, logs e tratamento de erro em um único arquivo. Antes de adicionar comandos locais, fallback OpenAI ou MCP, o bridge precisa virar um runtime modular e testável.

**O que entra:**

Reestruturar o bridge mantendo compatibilidade com a CLI atual:

- `bridge.py`: entrada CLI fina, parse de args, inicialização do runtime.
- `transport.py`: TCP/UART, frames, CRC8, handshake, recv/send, desconexão e reconexão.
- `device_protocol.py`: constantes `MSG_*`, `NB_EVT_*`, encode/decode e helpers para `SAY`, `EXPR`, `ACTION`, `GAZE`, `TEXT_SCROLL`.
- `voice_session.py`: buffer de áudio, `session_id`, timeout, métricas, snapshot imutável no `VOICE_END`.
- `stt.py`: Whisper/faster-whisper, normalização de ganho, filtros de qualidade e resultado estruturado.
- `tts.py`: Piper, checagem de binário/modelo e erros nomeados.
- `llm.py`: interface comum para provedores futuros (`gemini`, `openai`, `mock`, `none`).
- `runtime.py`: máquina de estados do bridge e orquestração da sessão.
- `config.py`: defaults, env vars e validação de startup.

Estados explícitos:

- `connecting`
- `idle`
- `receiving_audio`
- `transcribing`
- `routing`
- `thinking`
- `speaking`
- `degraded`
- `offline`

Observabilidade obrigatória:

- `session_id`
- duração total
- samples
- RMS/pico
- tempo STT
- backend/modelo STT
- rota escolhida (`discard`, `local_intent`, `llm`, `error`)
- provider/modelo LLM quando aplicável
- tempo LLM
- tempo TTS
- motivo final sempre nomeado

Robustez:

- `Ctrl+C` fecha socket, timers e threads de forma limpa.
- Reinício do ESP32 não exige reiniciar o bridge.
- Falha de Gemini/OpenAI/Piper/Whisper não encerra o processo.
- Sessão órfã expira e limpa buffers.
- `--dry-run` nunca chama LLM/TTS.
- `--llm none` permite operar somente com comandos locais.

Testabilidade:

- Testes unitários para encode/decode/CRC.
- Testes unitários para snapshot de sessão.
- Testes unitários para filtros de STT.
- Modo `--replay <arquivo.wav|arquivo.pcm>` para testar STT/routing sem hardware.
- Logs de replay devem ter o mesmo formato de uma sessão real.

**Critérios de aceitação:**

- [ ] O bridge novo substitui o antigo sem mudar firmware.
- [ ] `python bridge.py --host 192.168.1.23 --dry-run --whisper-backend faster --whisper-model small` continua funcionando.
- [ ] Uma sessão real gera uma linha final com `session_id`, rota e motivo.
- [ ] Reiniciar o ESP32 com o bridge aberto reconecta sem matar o processo.
- [ ] Falta de `GEMINI_API_KEY` inicia em modo degradado/local-only, não em falha opaca.
- [ ] Piper ausente gera erro nomeado e ACK limpo, sem crash.
- [ ] `--replay` permite reproduzir uma sessão STT sem robô.

---

### Etapa 12.14 — Local Intent Router v1 ✓

**Dependências:** 12.13 concluída
**Hardware necessário:** Não obrigatório para desenvolvimento; robô necessário para validação TTS real

**Contexto:** A documentação do StackChan/XiaoZhi separa comandos simples de diálogo complexo. O NoiseBot deve responder localmente a comandos básicos de produto, reduzindo custo, latência e dependência de quota.

**O que entra:**

Normalização pt-BR:

- minúsculas;
- remoção de acentos;
- remoção de pontuação;
- compactação de espaços;
- tolerância a erros comuns do Whisper;
- aliases de comandos por intenção.

Intenções locais v1:

- Hora:
  - "que horas são"
  - "hora atual"
  - "me diga as horas"
  - variações ruins como "e horas são agora"
- Status:
  - "qual seu status"
  - "você está bem"
  - "como você está"
- Rede/bridge:
  - "qual seu ip"
  - "você está conectado"
  - "teste o bridge"
  - "você está me ouvindo"
- Sono:
  - "dorme"
  - "vai dormir"
  - "acorda"
- Volume lógico:
  - "volume 80"
  - "aumente o volume"
  - "diminua o volume"
- Luz básica:
  - "mude a luz para azul/vermelho/verde"
- Movimento básico:
  - "olhe para esquerda/direita/cima/baixo"

Contrato de resposta de intent:

```python
{
    "intent": "local_time",
    "confidence": 0.0,
    "reply": "Agora são 23 horas e 42 minutos.",
    "expression_id": 2,
    "action": 0,
    "emot_event": 2,
    "device_commands": []
}
```

Regras:

- Intenção local com confiança alta não chama LLM.
- Intenção local ambígua pode pedir confirmação curta.
- Comando desconhecido cai para LLM se disponível.
- Com LLM indisponível, comando desconhecido responde erro amigável via TTS se possível.

**Critérios de aceitação:**

- [x] "Que horas são?" responde sem Gemini/OpenAI.
- [x] "E horas são agora" roteia para `local_time`.
- [x] `--dry-run` loga `route=local_intent intent=local_time` sem chamar Piper.
- [x] Modo real fala a hora via Piper.
- [x] Comando desconhecido cai para LLM somente quando `--llm` está ativo.
- [x] Zero chamadas LLM para hora/status/IP/teste bridge em 20 execuções.

**Validação pós-12.14 (2026-04-23):**

- Router local validado em `--dry-run` com `route=local_intent`, `intent=local_time`, `llm=none/none/0ms` e `motivo=descartado:dry_run_ok`.
- Modo real validado com Piper: robô falou resposta de hora sem chamar LLM.
- Falha de Piper agora gera `tts_indisponivel` com ACK limpo, sem derrubar a sessão.
- Voz Piper `pt_BR-faber-medium` normalizada para o contrato do firmware: reamostragem 22050 Hz → 16000 Hz e limiter de pico padrão em 8000 para evitar saturação.
- Modelos locais `.onnx` do Piper ficam fora do Git; instalação de voz é dependência local do bridge.

---

### Etapa 12.15 — Device Commands v1 ✓

**Dependências:** 12.14 concluída; protocolo bridge 12.1 estável
**Hardware necessário:** Robô completo para validação de face/LED/motion/audio

**Contexto:** StackChan documenta comandos para controlar speaker, motor, RGB, câmera e bateria. O NoiseBot deve começar pelo que já existe no protocolo atual e só ampliar o firmware quando houver necessidade concreta.

**O que entra:**

Dispatcher de comandos de dispositivo no bridge:

- `set_expression(expression_id, duration_ms)`
- `play_action(action_id)`
- `set_gaze(x, y)`
- `emit_emotion_event(event_id)`
- `scroll_text(text)`
- `set_volume(percent)` se o firmware expuser suporte.
- `set_led_color(color)` se o protocolo atual suportar, ou planejar `MSG_LED` mínimo em etapa própria.

Comandos v1:

- "olhe para a esquerda/direita/cima/baixo"
- "fique feliz/curioso/sonolento"
- "balance a cabeça"
- "diga que está ouvindo"
- "volume X%" quando suportado
- "luz azul/vermelha/verde" quando suportado

Regras:

- Comando suportado executa sem LLM.
- Comando reconhecido mas ainda não suportado responde honestamente:
  - "Eu entendi, mas ainda não tenho esse controle ligado."
- Nenhum comando local deve deixar o robô preso em `RESPONDING`.

**Critérios de aceitação:**

- [x] "Olhe para a esquerda" envia `MSG_GAZE` ou ação equivalente sem LLM.
- [x] "Fique feliz" envia `MSG_EXPR` sem LLM.
- [x] Comando não suportado loga `unsupported_device_command`.
- [x] 20 comandos locais consecutivos não deixam socket, sessão ou estado presos.
- [x] O bridge diferencia `intent_local_text` de `intent_device_command` nos logs.

**Início 12.15 (2026-04-23):**

- Dispatcher de comandos criado no bridge para o protocolo já existente: `MSG_GAZE`, `MSG_EXPR`, `MSG_ACTION`, `MSG_EMOT_EVENT` e `MSG_TEXT_SCROLL`.
- Router local passou a mapear "olhe para esquerda/direita/cima/baixo", "fique feliz/curioso/sonolento" e "balance a cabeça" para comandos suportados sem LLM.
- Volume e LED continuam reconhecidos como comandos locais, mas permanecem `unsupported_device_command` até o firmware expor contrato específico.
- Logs de sessão agora incluem `intent_kind=local_text|device_command` para separar resposta textual local de comando físico.

**Validação parcial 12.15 (2026-04-23):**

- Teste real com bridge conectado validou `local_device_move` para "Olhe para direita" e "Olha para a esquerda": ambos registraram `device_command_executed name=look` e `motivo=ok`, sem LLM.
- Teste real validou `local_device_expression` para "Fique feliz": registrou `device_command_executed name=set_expression` e `motivo=ok`, sem LLM.
- A frase "balance a cabeça" foi transcrita como `Balão se acabece`; o router passou a aceitar esse artefato fonético para despachar `play_action` sem cair no LLM.
- Ainda falta rodada longa de 20 comandos consecutivos e validação explícita do log `unsupported_device_command`.

**Estabilização de escuta/bridge (2026-04-24):**

- Firmware validado com `preroll=20`, cobrindo melhor fala emendada logo após o wake word.
- Sessão de escuta passou a logar `bridge_conn=0|1` na abertura; quando a fala começa sem bridge conectado, a sessão encerra como `bridge_disconnected` em vez de simular captura local descartável.
- Áudio enviado ao bridge ficou em caminho próprio com ganho controlado de `+12 dB`, limiter e diagnóstico `bridge tx diag`.
- Bridge local passou a reconectar em 1s e usar timeout de envio separado, reduzindo janela sem conexão após queda.

---

### Etapa 12.16 — LLM Providers e Fallback ✓

**Dependências:** 12.13 concluída; 12.14 concluída
**Hardware necessário:** Não obrigatório para desenvolvimento; robô para teste final

**Contexto:** O Gemini funcionou, mas mostrou erro `429` de quota. O produto não deve depender de um único provedor. Ao mesmo tempo, ChatGPT Pro não cobre uso de API; a integração OpenAI deve ser opcional e explícita.

**O que entra:**

Interface comum:

```python
class LlmProvider:
    def generate(self, prompt, status, tools=None) -> LlmResult:
        ...
```

Providers:

- `gemini`
- `openai`
- `mock`
- `none`

CLI/env:

- `--llm gemini|openai|mock|none`
- `--fallback-llm gemini|openai|mock|none`
- `GEMINI_API_KEY`
- `OPENAI_API_KEY`
- `NOISEBOT_GEMINI_MODEL`
- `NOISEBOT_OPENAI_MODEL`

Tratamento:

- `429`/quota: erro nomeado e fallback se configurado.
- timeout: erro nomeado e fallback se configurado.
- API key ausente: provider indisponível no startup, não durante a conversa.
- resposta vazia: erro nomeado.

**Critérios de aceitação:**

- [ ] `--llm gemini` preserva comportamento atual.
- [ ] `--llm none` funciona para intenções locais.
- [ ] Gemini 429 não mata o bridge.
- [ ] Gemini 429 cai para fallback quando `--fallback-llm openai` estiver configurado.
- [ ] Logs incluem `llm_provider`, `llm_model`, `llm_ms`, `llm_error`.
- [ ] Sem API keys, o bridge inicia em modo local-only com aviso claro.

---

### Etapa 12.17 — Feedback de Produto e Erros Sem Silêncio ✓

**Dependências:** 12.13 concluída; 12.15 desejável
**Hardware necessário:** Robô completo

**Contexto:** Um produto conversacional não pode deixar o usuário sem feedback quando está pensando, transcrevendo ou com erro de nuvem. StackChan usa LED para listening/speaking; NoiseBot deve usar face, LED, texto e sons curtos conforme seus recursos.

**O que entra:**

Feedback por estado:

- `listening`: expressão focada/curiosa, LED de escuta.
- `transcribing`: micro feedback visual curto.
- `thinking`: expressão pensativa, ação leve ou LED.
- `speaking`: expressão viva, LED de fala.
- `error_api`: expressão confusa e fala curta.
- `offline`: expressão curiosa/sonolenta e mensagem local.

Respostas locais de erro:

- STT baixa confiança:
  - "Eu ouvi, mas não entendi direito."
- LLM quota:
  - "Entendi sua pergunta, mas meu cérebro online recusou a resposta agora."
- LLM offline:
  - "Estou sem acesso ao cérebro online agora."
- TTS indisponível:
  - log explícito e feedback visual sem crash.

**Critérios de aceitação:**

- [x] Falha de Gemini 429 não termina em silêncio absoluto.
- [x] STT descartado por `logprob` ruim gera feedback visível ou sonoro controlado, sem chamar LLM.
- [x] `thinking` aparece quando LLM demora mais que 1s.
- [x] `speaking` inicia antes/durante envio de `SAY`.
- [x] Usuário consegue distinguir "não ouvi", "não entendi" e "nuvem falhou".

---

### Etapa 12.18 — Métricas de Produto e Harness de Regressão ✓

**Dependências:** 12.13 concluída; 12.14 concluída
**Hardware necessário:** Robô para UAT; PC para replay offline

**Contexto:** Para aproximar a experiência de StackChan/XiaoZhi, precisamos medir latência, estabilidade e qualidade de rota. Testes manuais isolados ajudam, mas não protegem contra regressões no bridge.

**O que entra:**

Métricas por sessão:

- wake → `PODE FALAR` (firmware/log serial, quando disponível);
- `VOICE_END` → fim STT;
- `VOICE_END` → rota escolhida;
- `VOICE_END` → início TTS;
- `VOICE_END` → primeiro `SAY`;
- duração TTS;
- motivo final.

Harness:

- Pasta de fixtures com WAV/PCM curtos:
  - hora;
  - status;
  - ruído;
  - silêncio;
  - comando de movimento;
  - frase longa;
  - fala baixa.
- `--replay` com resultado estruturado.
- Teste de regressão para local intents.
- Checklist manual de hardware.

Metas de produto:

- Comando local simples: resposta começa em < 1.5s após `VOICE_END`.
- STT faster/small: < 1.2s para frases curtas.
- 20 comandos locais consecutivos: zero crash.
- 10 sessões LLM consecutivas: zero sessão travada.
- Falso comando local em ruído/silêncio: 0 em fixtures.

**Critérios de aceitação:**

- [ ] Fixtures offline cobrem hora, status, ruído, silêncio e comando de corpo.
- [x] `--replay` retorna resultado estruturado com rota, outcome e detalhe diagnóstico.
- [ ] Métricas aparecem no log final de cada sessão.
- [ ] Checklist de hardware documenta comandos, resultado esperado e logs-chave.
- [ ] Antes de mexer em LLM ou protocolo, replay precisa continuar verde.

---

### Etapa 12.19 — Conversation Protocol v2

**Dependências:** 12.13 concluída; 12.15 validada; 12.18 desejável
**Hardware necessário:** Robô + bridge local para validação; PC para testes de protocolo

**Contexto:** XiaoZhi usa um contrato explícito de conversa (`hello`, `listen/detect`, `listen/start`, `listen/stop`) e audio channel sob demanda. O NoiseBot hoje usa um protocolo funcional, mas ainda muito próximo de PCM/eventos de protótipo. Esta etapa define uma versão v2 sem quebrar a v1.

**Status atual:** concluída para contrato batch/half-duplex e em regressão
automática. O bridge mantém handshake v1 vazio e anuncia `HELLO` v2 em runtime;
firmware atualizado responde com capabilities v2 quando recebe esse `HELLO`,
preservando compatibilidade com bridge v1. A telemetria v2 de sessão já registra
e envia via `MSG_SESSION` os eventos `WAKE_DETECTED`, `LISTEN_START`,
`LISTEN_STOP`, `TRANSCRIBE_START`, `THINKING_START`, `TTS_START`, `TTS_STOP`,
`SESSION_DONE` e `SESSION_ERROR`; o firmware recebe/loga de forma passiva. O
fake firmware em `bridge/tests/test_fake_firmware.py` cobre o protocolo sem
hardware: handshake, wake/listen/speak/idle, wake sem áudio, áudio fora de
sessão, frame corrompido, sessão vazia seguida de sessão válida, resposta longa
em chunks, STT rejeitado e falha de TTS. A suíte do bridge está verde com 135
testes.

**Limites explícitos:** follow-up automático permanece em standby e não deve ser
reativado como efeito colateral. Barge-in por wake word está validado; barge-in
automático por VAD sem wake e realtime continuam futuros. AEC de dispositivo
está bloqueado no hardware atual por falta de referência limpa de playback; AEC
server-side depende de referência/timestamps de playback que ainda não existem
no protocolo.

**O que entra:**

- Manter compatibilidade com protocolo atual (`MSG_AUDIO_CHUNK`, `MSG_EVENT`, `MSG_SAY`, etc.).
- Definir handshake v2:
  - versão de protocolo;
  - sample rate;
  - formato de áudio (`pcm16` fallback seguro, `opus` capability opt-in);
  - recursos do firmware (`say`, `expr`, `gaze`, `action`, `text_scroll`, `status`);
  - recursos do bridge (`stt`, `llm`, `tts`, `local_intents`, `tools`).
- Mensagens explícitas:
  - `WAKE_DETECTED`;
  - `LISTEN_START`;
  - `LISTEN_AUDIO`;
  - `LISTEN_STOP`;
  - `TRANSCRIBE_START`;
  - `THINKING_START`;
  - `TTS_START`;
  - `TTS_STOP`;
  - `SESSION_ERROR`;
  - `SESSION_DONE`.
- Estado de sessão com `session_id` dos dois lados.
- Motivo final obrigatório e padronizado.
- Compatibilidade v1/v2 negociada no handshake.

**Taxonomia v2 atual:**

- `end_reason`: motivo físico de encerramento (`silence`, `timeout`, `bridge_disconnected`, `cancelled`, `bridge_watchdog_timeout`).
- `outcome`: resultado de pipeline (`ok`, `dry_run_ok`, `audio_rejected`, `stt_rejected`, `stt_unavailable`, `llm_unavailable`, `tts_failed`, `llm_error`, `pipeline_error`, `session_timeout`).
- `detail`: detalhe diagnóstico preservado para tuning (`logprob_-1.43`, `audio_baixo_*`, `llm_indisponivel`, etc.).

**Critérios de aceitação:**

- [x] Bridge v2 conecta em firmware v1 sem regressão.
- [x] Firmware/bridge logam a versão negociada.
- [x] Uma sessão completa tem `WAKE_DETECTED -> LISTEN_START -> LISTEN_STOP -> SESSION_DONE`.
- [x] Queda de bridge durante sessão gera `SESSION_ERROR` nomeado e estado limpo.
- [x] O protocolo v2 pode ser testado em unidade sem hardware.
- [x] Firmware loga `MSG_SESSION` passivo em hardware após build/flash do commit `9f86b54`.
- [x] Fake firmware valida sessão completa, sessão vazia, áudio fora de sessão e
      frames corrompidos sem hardware.
- [x] Falhas de STT/TTS geram eventos terminais nomeados e não deixam sessão
      pendente.
- [x] Opus foi validado como capability experimental opt-in com fallback PCM16,
      `opus-live`, fake firmware Opus e testes de codec/adapter.
- [x] Barge-in por wake word e no-echo foram validados por harness live.
- [x] AEC-live classifica AEC de dispositivo como `promotable=false` quando o
      firmware retorna diagnóstico de falta de referência.
- [x] Opus foi promovido de experimento manual para capability oficial opt-in,
      com status/HELLO/metrics coerentes, worker Codec v2, default local do
      server e fallback PCM16 automatico.
- [ ] Reconexão TCP/UART coberta por teste automático.
- [ ] Cancelamento explícito de fala (`SPEECH_CANCEL`/turn id no fio) coberto
      por teste antes de qualquer nova mudança no firmware.

---

### Etapa 12.20 — Robot Tools v2 e Schemas de Segurança ✓

**Dependências:** 12.15 validada; 12.19 concluída
**Hardware necessário:** Robô para validação física

**Contexto:** StackChan expõe capacidades do robô como tools com nomes e descrições claras. O NoiseBot já tem intents locais e comandos básicos, mas precisa formalizar ferramentas com schema, limites e integração segura com o firmware. O bridge possui catálogo canônico, aliases para os comandos locais existentes, validação de schema antes de qualquer envio ao firmware e runtime local para status/lembretes sem LLM.

**O que entra:**

- Definir catálogo de tools:
  - `noisebot.robot.get_status`;
  - `noisebot.robot.set_gaze`;
  - `noisebot.robot.set_expression`;
  - `noisebot.robot.set_led_mood`;
  - `noisebot.robot.play_action`;
  - `noisebot.robot.create_reminder`;
  - `noisebot.robot.stop_reminder`;
  - `noisebot.robot.get_reminders`.
- Cada tool deve declarar:
  - nome;
  - descrição de uso;
  - schema de entrada;
  - limites físicos;
  - se exige motion safety;
  - se é local-only;
  - resposta esperada.
- Bridge valida schema antes de enviar comando.
- Firmware continua validando do lado seguro.
- Comandos de movimento só podem chegar ao `motion_service` por caminho autorizado e vetável.

**Critérios de aceitação:**

- [x] 10 comandos válidos executam sem LLM.
- [x] 10 comandos inválidos são rejeitados antes de chegar ao firmware.
- [x] Logs diferenciam `tool_call`, `tool_result`, `tool_rejected`.
- [x] Tool de movimento não passa por cima de `motion_safety`.
- [x] Lembretes locais funcionam sem LLM.

---

### Etapa 12.21 — Expressive Modifiers e Overlays ✓

**Dependências:** 12.15 validada; 12.19 validada em hardware; conductor/expression/gaze estáveis
**Hardware necessário:** Robô completo

**Contexto:** StackChan organiza expressividade em modificadores independentes (`Blink`, `Breath`, `IdleMotion`, `IdleExpression`, `HeadPet`, `Speaking`). O NoiseBot já tem serviços equivalentes, mas precisa formalizar overlays temporários para reduzir acoplamento e melhorar naturalidade. `ui_overlay_service` v1 criado como layer visual independente no `render_service`; comandos de volume/texto e eventos `SESSION v2` do bridge já geram feedback visual transitório sem substituir o baseline de `IDLE`. Toasts de sessão validados em hardware em 24/04/2026. Intents locais textuais agora também enviam feedback visual; respostas de hora usam card de relógio sem TTS redundante e textos `Volume NN%` reaproveitam o card de volume. Comandos de volume relativos aceitam `volume`/`som`, exibem porcentagem e disparam beep curto no nível final. Status local usa card visual com saúde/atenção quando disponíveis. Teste de bridge e status de rede/IP usam card visual sem TTS.

**O que entra:**

- Definir contrato de overlay:
  - tipo;
  - prioridade;
  - duração;
  - alvo (`expression`, `gaze`, `led`, `motion`, `text`);
  - política de saída;
  - se bloqueia ou compõe com idle.
- Overlays v1:
  - `product_overlay` para volume/texto/status local;
  - `listening_overlay`;
  - `thinking_overlay`;
  - `speaking_overlay`;
  - `touch_pet_overlay`;
  - `error_overlay`;
  - `reminder_overlay`.
- `IDLE` continua sendo baseline obrigatório.
- Overlays nunca substituem permanentemente expressão/gaze/postura base.
- Speaking overlay deve animar boca/expressão/LED/gaze sem depender de LLM.

**Mapeamento inicial proposto:**

- `LISTEN_START`: `listening_overlay` com expressão atenta/curiosa, LED de escuta e gaze estável.
- `TRANSCRIBE_START`: micro overlay de processamento curto, discreto, sem parecer resposta.
- `THINKING_START`: `thinking_overlay` com olhar/piscar de pensamento e LED suave.
- `TTS_START`: `speaking_overlay` com prioridade acima de idle e abaixo de safety/error.
- `TTS_STOP`: encerra `speaking_overlay` e devolve autoridade ao baseline do estado atual.
- `SESSION_ERROR`: `error_overlay` curto e recuperável, retornando para `IDLE`/estado anterior.
- `SESSION_DONE`: limpeza de overlays conversacionais transitórios.

**Critérios de aceitação:**

- [x] Ao entrar em `IDLE`, overlays transitórios são limpos.
- [x] Volume local exibe barra/porcentagem transitória sem TTS obrigatório.
- [x] Texto curto do bridge possui overlay visual transitório.
- [x] Intent local de hora exibe relógio transitório na tela.
- [x] Intent local de status exibe card visual sem TTS redundante.
- [x] Intents locais de bridge/rede exibem card visual sem TTS redundante.
- [x] Eventos `LISTEN_START`, `TRANSCRIBE_START`, `THINKING_START`, `TTS_START`, `SESSION_ERROR` e queda/timeout do bridge geram toast visual curto.
- [x] Speaking overlay inicia com `TTS_START` e termina com `TTS_STOP`.
- [x] Touch afetivo não abre escuta e não remove baseline permanentemente.
- [x] Erro de LLM/TTS gera feedback visível curto e volta ao idle.
- [x] 20 overlays consecutivos não deixam estado visual preso.

---

### Etapa 12.22 — Touch Semântico e Afetivo v2 ✓

**Dependências:** 12.10 concluída; touch_service estável
**Hardware necessário:** Touch de cobre do NoiseBot

**Contexto:** StackChan trata toque de cabeça como gesto afetivo, com press/release/swipe. O NoiseBot tem apenas um touch, mas ainda pode extrair semântica temporal e intensidade para enriquecer interação sem virar gatilho de escuta.

**O que entra:**

- Classificar:
  - tap curto;
  - toque longo;
  - carinho contínuo;
  - duplo tap;
  - sequência de taps;
  - intensidade forte/fraca quando disponível.
- Publicar eventos semânticos de touch.
- Integrar com emotion_model, conductor, LTM e attention_service.
- Touch não inicia listening.
- Touch pode interromper overlay leve, mas não deve derrubar sessão crítica sem regra explícita.

**Implementado:**

- `touch_semantic_service` classifica TAP, DOUBLE_TAP, LONG_PRESS, SUSTAINED, WARM_PULSE (3–8s), DEEP (>8s) e CARESS (>15s).
- `behavior_engine` conecta todos os eventos ao `emotion_model`, `conductor` e `ltm_record`.
- `state_machine`: TAP e LONG_PRESS em `ATTENTIVE`/`RESPONDING` não mudam estado — reação afetiva acontece via event bus sem derrubar sessão.
- `long_term_memory`: adicionados `LTM_IACT_TOUCH_DOUBLE_TAP`, `LTM_IACT_TOUCH_DEEP` e `LTM_IACT_TOUCH_CARESS`; DEEP conta como 2 e CARESS como 3 na familiaridade.
- Milestone de 50 toques atualizado para `>= 50` e cobre todos os tipos de toque afetivo.

**Critérios de aceitação:**

- [x] Tap curto gera reação afetiva curta.
- [x] Toque longo gera reação diferente de tap.
- [x] Sequência de taps não abre bridge.
- [x] Touch durante listening não corrompe sessão de voz.
- [x] LTM registra interação semântica, não apenas contador bruto.

**Validado em hardware (2026-04-25).**

---

### Etapa 12.23 — Setup e Diagnóstico de Produto ✓

**Dependências:** 15.1 desejável; 12.18 desejável
**Hardware necessário:** Robô completo

**Contexto:** StackChan tem fluxo de setup e app de produto. Para o NoiseBot, o primeiro passo adequado é um dashboard local profissional no bridge/app externo para diagnóstico, calibração e testes repetíveis, sem depender de app mobile ou cloud e sem consumir SRAM do firmware.

**O que entra:**

- Dashboard externo de diagnóstico:
  - estado atual;
  - bridge conectado/desconectado;
  - wake threshold/modelo;
  - audio RMS/pico;
  - último motivo de sessão;
  - PSRAM/SRAM;
  - FPS;
  - saúde do watchdog;
  - contadores de touch/wake/voice.
- Testes guiados:
  - touch;
  - wake word;
  - VAD/listening;
  - bridge roundtrip;
  - TTS/SAY;
  - expression/gaze/LED;
  - motion safety quando liberado.
- Exportar snapshot de diagnóstico para SD.

**Implementado:**

- `GET /api/diag` — JSON unificado com state, bridge (connected/transport/protocol_v/last_rx_ms), wake (model/threshold/detections), audio (rms/listening), memory (psram/dram), fps, health, uptime, touch_count, sessions, hours_alive.
- `POST /api/diag/snapshot` — chama `diagnostics_dump_to_sd()` e retorna JSON com versão, health, config resumida e últimas 5 linhas do log ring.
- `GET /api/diag/test/wake` — modelo WakeNet9, keyword "Hi ESP", threshold, active, detections da sessão.
- `GET /api/diag/test/bridge` — connected, transport, protocol_v, last_rx_ms, porta TCP.
- Novos getters: `wake_service_get_detect_count()`, `wake_service_get_threshold()`, `bridge_service_get_protocol_version()`, `bridge_service_get_last_rx_age_ms()`.
- Todos os handlers são não-bloqueantes; snapshot chama `diagnostics_dump_to_sd()` diretamente do HTTP handler (prioridade baixa do task httpd, sem impacto em tasks críticas).

**Critérios de aceitação:**

- [x] Usuário consegue validar áudio/bridge sem ler 200 linhas de serial.
- [x] Snapshot de diagnóstico inclui versão, config e últimos erros.
- [x] Teste de wake word mostra modelo, threshold e resultado.
- [x] Teste de bridge mostra latência e versão de protocolo.
- [x] Nenhum teste de dashboard externo bloqueia boot ou tasks críticas.

---

## BLOCO 13 — Visão por Computador

> Objetivo: Usar a câmera (8.1) para detecção de presença, face tracking e
> gestos simples. Sem ML pesado — algoritmos clássicos dentro dos limites
> do ESP32-S3.

---

### Etapa 13.0 — Observação Visual Básica ✓ parcial

**Dependências:** 8.1 funcional, 15.1 Companion API local
**Hardware necessário:** Câmera OV2640

**Objetivo:** transformar a captura bruta da câmera em telemetria visual simples
e confiável antes de acionar comportamento autônomo.

**Implementado / validado em hardware (2026-05-25):**

- `vision_service` expõe observação pontual sem manter a câmera ligada em boot.
- `GET /api/vision/observe` retorna JSON com:
  - `valid`, `scene`, `timestamp_ms`;
  - `width`, `height`, `jpeg_bytes`, `capture_ms`;
  - `luma_avg`, `luma_min`, `luma_max`, `contrast`;
  - `motion_score`.
- Bridge v2 consome a observação para intents locais:
  - "o que você está vendo?"
  - "como está a luz?"
  - "tem movimento?"
  - "você está me vendo?"
- Bridge v2 tem analisador visual opcional (`bridgev2.vision.analyzer`) que baixa
  o JPEG de `/api/camera/snapshot` e executa detecção real de rosto via OpenCV
  Haar cascade quando a extra `vision` está instalada.
- A resposta continua honesta: se o detector do bridge não estiver disponível,
  o robô informa que só tem luz/contraste/movimento; se detectar rosto, responde
  posição aproximada no frame.

**Critérios de aceitação:**

- [x] `/api/vision/observe` retorna observação válida em 640×480.
- [x] Bridge v2 responde perguntas locais de visão sem chamar LLM.
- [x] Camera, bridge e TTS operam no mesmo firmware sem erro imediato.
- [x] Bridge v2 possui caminho de visão real para rosto em snapshot JPEG
      (`pip install .[vision]` no ambiente do bridge).
- [ ] Métricas de visão registradas no snapshot de diagnóstico.
- [ ] Observação visual repetida por 30 minutos sem degradação de heap ou latência.

---

### Etapa 13.1 — Detecção de Presença (Layer 4)

**Dependências:** 13.0 validada, 10.1 (attention_service)
**Hardware necessário:** Câmera OV2640

**O que entra:**

- `vision_service` em `components/services/vision_service/`:
  - Frame differencing temporal e/ou heurística de região central sobre a observação 640×480.
  - Presence score: combinação de movimento, contraste, estabilidade e mudança sustentada.
  - `NB_EVT_PRESENCE_DETECTED`, `NB_EVT_PRESENCE_LOST`.
  - Frame de referência atualizado apenas quando a cena estiver estável.
  - Debounce temporal para não confundir sombra/iluminação com presença.

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

### Etapa 13.2 — Face Tracking (Layer 4/5 + Bridge)

**Dependências:** 13.1 concluída
**Hardware necessário:** Câmera OV2640

**O que entra:**

- Extensão de `vision_service`:
  - Detecção de rosto por segmentação de cor pele (YCbCr thresholds) + análise de forma.
  - Sem CNN/ML — algoritmo clássico que cabe em < 50KB de código.
  - Output: posição normalizada do rosto detectado (-1.0 a 1.0 em x e y).
  - Confiança da detecção (0.0–1.0).
  - `NB_EVT_FACE_DETECTED` (data: posição), `NB_EVT_FACE_LOST`.
- Caminho bridge-first já iniciado:
  - `VisionClient.analyze()` usa snapshot JPEG + OpenCV Haar cascade no PC.
  - Intents como "você está me vendo?" passam a responder com detecção real
    quando o detector está disponível.
  - Próximo passo: transformar esse resultado em comando de gaze/atenção para
    o firmware sem manter câmera em loop contínuo.

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

> Objetivo: O robot expõe uma API HTTP local para configuração, monitoramento
> e controle remoto. A interface visual roda fora do firmware, no bridge/app de
> desenvolvimento, para preservar SRAM, sockets e stack do ESP32-S3. WiFi já
> está ativo desde a Etapa 9.6 — este bloco constrói os serviços de aplicação
> sobre essa infraestrutura.

**Restrições de hardware para este bloco (ESP32-S3):**

- TLS/HTTPS: mbedTLS consome ~250 KB SRAM adicionais — inviável. HTTP na LAN apenas.
- Sem WebSocket no firmware. Atualização em tempo real fica no bridge/app externo.
- Sem streaming de áudio ou vídeo via WiFi (jitter e banda insuficientes).

**Orçamento de SRAM incremental (além do wifi_service da 9.6):**

| Componente              | SRAM estimada |
| ----------------------- | ------------- |
| esp_http_server (3 cx)  | ~12-16 KB     |
| JSON/handlers REST      | ~4 KB         |
| **Total incremental**   | **~16-20 KB** |

---

### Etapa 15.1 — Companion API HTTP (Layer 2)

**Dependências:** 9.6 concluída (IP adquirido)

**Hardware necessário:** Não

**O que entra:**

- `web_service` em `components/infra/`:
  - `esp_http_server` com máximo 3 conexões HTTP simultâneas.
  - Sem HTML, CSS, JS ou WebSocket embutidos no firmware.
  - Dashboard visual roda no bridge/app externo e consome apenas endpoints REST.
  - Iniciado somente após `NB_EVT_WIFI_IP_ACQUIRED` — nunca bloqueia o boot.

**REST API:**

| Endpoint            | Método | Descrição                                               |
| ------------------- | ------ | ------------------------------------------------------- |
| `GET /api/status`   | HTTP   | JSON: state, expression, attention, health, uptime, fps |
| `GET /api/persona`  | HTTP   | JSON: warmth, energy, curiosity, trust                  |
| `GET /api/config`   | HTTP   | JSON com todas as chaves NVS relevantes                 |
| `POST /api/config`  | HTTP   | Atualiza chave NVS (body: `{"key":"val","value":x}`)    |
| `POST /api/command` | HTTP   | Injeta ação (body: `{"type":"ACTION","value":"GREET"}`) |

**Sem autenticação no protótipo** (LAN local, sem exposição externa).

**Critérios de aceitação:**

- [x] `GET /api/status`: JSON válido retornado em < 100ms
- [x] `POST /api/command` GREET: `conductor_play(GREET)` executado em < 300ms
- [x] `POST /api/config` volume: `config_set_volume()` persistido e efetivo sem reiniciar
- [x] FPS de render ≥ 25fps com cliente REST consultando periodicamente
- [x] Dashboard externo reiniciado/desconectado: sem crash, nova consulta REST aceita normalmente

---

### Etapa 15.2 — OTA e Backup de Personalidade

**Dependências:** 15.1 concluída
**Hardware necessário:** Não

**O que entra:**

- **OTA via HTTP** usando WiFi já ativo da Etapa 9.6:
  - Endpoint `POST /api/ota` recebe URL de firmware `.bin` (servidor local ou S3).
  - `esp_ota` com validação de magic bytes antes de aplicar.
  - Robot entra em `NB_STATE_OTA` durante update: motion off, LEDs laranja pulsante, progresso registrado em log/API.
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

### Etapa 16.1 — Wake Word Customizada (Layer 4)

**Dependências:** 12.6 concluída (wake word "Hi ESP" validada em produção)
**Hardware necessário:** INMP441 já conectado

**Contexto:** A Etapa 12.6 integrou ESP-SR com wake word pronta ("Hi ESP"). Esta etapa substitui por uma keyword customizada do NoiseBot. Não implementar antes de 12.6 estar estável — o treinamento/customização é trabalho separado da integração de infraestrutura.

**O que entra:**

- Keyword customizada: **"Oi NoiseBot"** ou **"Hey NoiseBot"** via ESP-SR Customization Tool.
  - "Noise" sozinho não é suficiente — muito curto e ambíguo em ambiente real.
  - Processo: gravar amostras, treinar com ESP-SR tool, exportar modelo `.bin`, incluir no firmware.
- `wake_service` atualizado para carregar modelo customizado de `/sdcard/models/wake.bin` se presente, senão fallback para "Hi ESP" embutido.
- NVS: `nb_svc/ww_model` — path do modelo customizado.
- Dashboard externo: seção wake word — modelo ativo, threshold, botão de teste, enable/disable.

**Critérios de aceitação:**

- [ ] Dizer "Oi NoiseBot": `NB_EVT_WAKE_WORD_DETECTED`, bot entra em ATTENTIVE em < 1.5s
- [ ] Dizer "Oi NoiseBot" em SLEEPING: acorda e entra em ATTENTIVE
- [ ] Ruído ambiente e TV sem keyword: zero false positives em 10 minutos
- [ ] Modelo ausente no SD: fallback para "Hi ESP" sem crash, log informativo
- [ ] `ww_enabled=0` em NVS: keyword desabilitada, toque continua funcionando
- [ ] Threshold ajustável pelo dashboard externo sem reflash

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
- Dashboard externo: seção TTS — host/porta/speaker, botão de teste com campo de texto.
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
- Dashboard externo: seção "Choreography" com 3 botões de play + campo de sequência custom em JSON.
- API `/api/choreo` (POST): `{"steps":[{"action":"GREET","delay_ms":500},...]}`

**Critérios de aceitação:**

- [ ] `NB_CHOREO_DANCE` executada: 8 ações na ordem correta, timing dentro de ±50ms
- [ ] Choreo interrompível: `conductor_stop_choreo()` para no step atual sem travar conductor
- [ ] Choreo via dashboard externo: POST JSON → sequência executa no robot
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
- Dashboard externo: botão "Photo Frame" + upload de JPEG via `POST /api/photos`.

**Critérios de aceitação:**

- [ ] 10 fotos no SD → exibidas em loop, intervalo correto
- [ ] Toque durante slideshow → retorna para IDLE em <500ms
- [ ] JPEG inválido ou corrompido → skipped, próxima foto sem panic
- [ ] `photo_interval_s` alterado via dashboard externo sem reflash
- [ ] Upload de foto via `/api/photos` → aparece no próximo ciclo do slideshow

---

### Etapa 18.2 — Camera Stream Externo (Layer 2/4)

**Dependências:** 8.1 (câmera OV2640), 15.1 (web_service) concluídas
**Hardware necessário:** câmera OV2640 conectada no DVP

**O que entra:**

- Streaming contínuo não roda dentro do firmware. O firmware fornece snapshot/observação,
  e o bridge/app externo monta preview por polling controlado ou por stream próprio.
- `GET /api/camera/snapshot`: JPEG único, útil para polling de baixa frequência.
- `POST /api/camera/config`: `{"resolution":"QVGA","quality":10}` — ajusta encoder JPEG.
- Integração com `face_tracking_service` (Etapa 13.2): o endpoint de observação retorna bounding box/estado de face quando disponível.
- Dashboard externo: seção "Camera" com preview por snapshot e botão de captura.

**Critérios de aceitação:**

- [ ] Preview externo por snapshot/polling funciona sem OOM ou watchdog
- [ ] Snapshot: JPEG válido retornado em <500ms
- [ ] Preview ativo não degrada FPS do display (render_service isolado)
- [ ] Segundo cliente externo não força estado persistente extra no firmware

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
| CÂMERA ATIVA         | Etapa 8.1      | OV2640 captura 640×480 sob demanda com sessão V4L2              |
| HARDWARE EXPANDIDO   | Etapa 8.3      | IMU, sensores ambientais e bateria ativos e integrados          |
| STACK COMPLETA       | Etapa 9.6      | Todos os serviços da arquitetura existem, WiFi ativo            |
| OUVIDOS INTELIGENTES | Etapa 10.4     | Robot distingue tipo, tom e padrão de estímulos                 |
| PERSONALIDADE VIVA   | Etapa 11.4     | Comportamento perceptivelmente diferente após 1 semana de uso   |
| ROBOT CONVERSADOR    | Etapa 12.2     | Conversa completa com LLM: fala → entende → responde → expressa |
| ROBOT OBSERVADOR     | Etapa 13.0     | Responde sobre cena, luz e movimento sem LLM                    |
| ROBOT VIDENTE        | Etapa 13.3     | Olha para quem está na frente, reage a gestos                   |
| ROBOT CONECTADO      | Etapa 15.2     | Companion API ativa, OTA funcional, personalidade portável      |
| ROBOT EXPRESSIVO+    | Etapa 16.4     | Fala, ruboriza, dança — expressividade completa                 |
| ROBOT AGENTE         | Etapa 17.1     | LLM aciona hardware durante resposta — age enquanto pensa       |
| ROBOT VISUAL         | Etapa 18.2     | Câmera ao vivo no browser, fotos no display                     |
