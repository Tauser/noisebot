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

**O que entra:**

- `emotion_model`: vetor (valência, ativação) em [-1, 1]. Atualização gradual. Decaimento para neutral. Mapeamento para `face_state_t` e parâmetros de movimento.
- `state_machine`: estados BOOT_UP, IDLE, ATTENTIVE, RESPONDING, TOUCH_REACTING, SLEEPING, ERROR, SAFE_MODE. Transições via eventos.

**Critérios de aceitação:**

- [ ] Todas as transições de estado loggadas com motivo e timestamp
- [ ] Emotion decai para neutral após 60s: verificado
- [ ] 9 emoções → 9 faces distinguíveis: verificado visualmente
- [ ] Timeout IDLE → SLEEPING: configurável via NVS, funcionando

---

### Etapa 5.2 — Gaze System e Idle Behavior

**Dependências:** 5.1 concluída, 3.3 concluída
**Hardware necessário:** Sim

**O que entra:**

- `gaze_service`: saccade model (movimento rápido + overshoot + settle), micro-drift gaussiano, aversive gaze periódico. Gaze reinterpretado no modelo EMO: sem pupila — afeta microdeslocamento do shape do olho (y_l/y_r, x_off) e leve tilt de pescoço. Gaze lidera o pescoço por 100ms.
- `idle_service`: microbehaviors probabilísticos — blink (Poisson, µ=4s), micro-saccade (5-15s), micro-neck-movement (10-30s), LED breathing (4s), yawn ocasional.

**Critério subjetivo obrigatório:**

- [ ] Observar robot em idle por 2 minutos: parece **vivo**, não mecânico

**Critérios mensuráveis:**

- [ ] Intervalo de blinks: nenhum <1.5s, nenhum >10s em observação de 5min
- [ ] Micro-movements de pescoço: amplitude <5°, frequência ≤ 3/minuto
- [ ] Aversive gaze: olhar se desvia a cada 8-15s em modo ATTENTIVE

---

### Etapa 5.3 — Expression System

**Dependências:** 5.2 concluída
**Hardware necessário:** Sim

**O que entra:**

- `expression_service`: mantém current/target face_state, interpola a cada frame, `expression_play(expr, ms)`, `expression_set_base(expr)`, fila de expressões.
- Mapeamento de eventos → expressões (touch, voz, estado).
- Blink drive com curva de Bezier (fechamento mais rápido que abertura).

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

**Critério de qualidade:**

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

**Critérios de aceitação:**

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

- `long_term_memory`: `interaction_history` (ring buffer 200 entradas, binário compacto), `persona_state` (JSON pequeno), `event_journal` (1000 entradas rotativas), `usage_stats`.
- Flush para SD a cada 5min ou ao entrar em SLEEPING.
- `behavior_engine` consulta LTM nas transições de estado.
- API: `ltm_get_total_touch_count()`, `ltm_get_hours_alive()`, `ltm_is_user_familiar()`.

**Critérios de aceitação:**

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

## BLOCO 8 — Expansões Futuras

### Etapa 8.1 — Câmera OV2640

Pré-requisito: Bloco 7 concluído, 300KB PSRAM headroom verificado.
Ações: inicializar DVP, alocar frame buffer em PSRAM, detecção de rosto básica.

### Etapa 8.2 — IMU MPU-6050

Pré-requisito: Câmera estabilizada.
Ações: inicializar I2C0, driver MPU-6050, tap detection por hardware, detecção de movimento externo.

### Etapa 8.3 — Bateria e Gestão de Energia

Pré-requisito: Hardware de bateria presente (nova versão de placa).
Ações: bq25185, MAX17048, TPS61088, power manager completo, modos de economia.

---

## Resumo de Marcos

| Marco            | Bloco          | Indicador                                              |
| ---------------- | -------------- | ------------------------------------------------------ |
| BASE SÓLIDA      | Fim do Bloco 0 | Boot determinístico, watchdog, NVS, SD, event bus      |
| DISPLAY PRONTO   | Etapa 1.3      | Face EMO com 9 expressões, blink assimétrico, FPS ≥ 30 |
| MOTION SAFE      | Etapa 3.2      | Todos os critérios de safety verificados               |
| ROBOT EXPRESSIVO | Etapa 5.4      | Conductor funcionando, outputs coordenados             |
| PRODUTO INICIAL  | Etapa 6.1      | 1h sem panic, latência OK, temperatura OK              |
| PRODUTO MADURO   | Etapa 7.3      | 8h contínuas, 100 power cycles, testes de produto      |
