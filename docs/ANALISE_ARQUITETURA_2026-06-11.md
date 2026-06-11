# Análise de Arquitetura — NoiseBot Firmware

**Data:** 2026-06-11 · **Escopo:** firmware ESP32-S3 (`components/`, `main/`, sdkconfig, partições)
**Método:** leitura integral de event_bus, motion_safety, servo_hal, power_monitor, watchdog_service, main.c, servo_test.c; leitura dirigida de boot_manager, web_service, motion_service, idle_service, state_machine, audio_hal, display_hal, render_service.

Cada item traz: problema → risco → como corrigir (sem código).

---

## P0 — Críticos de Safety

### 1. `servo_test.c` move servos a cada boot, antes de qualquer safety
`app_main()` chama `nb_servo_test_ping()` **antes** de `boot_manager_run()`. Com `NB_SERVO_TEST_ENABLE_MOTION=1` (estado atual), todo boot executa centro→min→centro→max→centro nos dois servos sem motion_safety, sem limites de config, sem stall detection. Viola a regra inegociável "nenhum movimento antes de motion_safety verde".

**Correção:** remover a chamada de `main.c`. Mover servo_test para um componente de diagnóstico ativável só por opção Kconfig (`CONFIG_NB_DIAG_SERVO`) que, quando ligada, ainda exija motion_safety armado, ou rode apenas em build de bancada nunca flashada no robô montado. Adicionar verificação de build (CI/script) que falhe se `servo_test.h` for incluído em `main.c`.

### 2. Safety desligado no boot enquanto caminhos de bypass continuam ativos
`phase_safety()` e `phase_motion()` fazem `phase_skip("motion desativado temporariamente")` — `motion_safety_init()` e `arm()` nunca rodam. Resultado: heartbeat, stall detection, veto e brownout-disable são dead code em produção. Ao mesmo tempo, dois caminhos **sem** safety movem servos: o servo_test (item 1) e o endpoint HTTP de calibração (item 3). A inversão é total: o caminho seguro está desligado e os inseguros, ligados.

**Correção:** reativar PHASE_SAFETY imediatamente (init da safety_task é inofensivo mesmo sem arm). Enquanto PHASE_MOTION estiver suspensa, nenhum outro caminho pode escrever em servo — remover/guardar os bypasses. Tratar "motion desativado temporariamente" como flag de config visível em diagnostics, não como skip silencioso de fase.

### 3. Endpoint HTTP de calibração move servos com bypass deliberado do safety, sem autenticação
`web_service.c` (~linha 1100) chama `servo_hal_enable_torque`/`servo_hal_write_position` direto, 8× fire-and-forget, com comentário admitindo "bypass de safety intencional". O servidor HTTP não tem autenticação. Qualquer dispositivo na LAN pode comandar torque e posição ignorando limites, estado FAULT e velocidade.

**Correção:** calibração deve passar por `motion_service` em um "modo calibração" explícito: safety continua checando (limites largos de calibração, velocidade reduzida), exige confirmação física no robô (ex.: toque longo) para entrar no modo, expira por timeout (o timer de 10s já existe — mantê-lo). Adicionar autenticação mínima ao HTTP (token gerado no primeiro boot, guardado em NVS, exibido no display). Proibir include de `servo_hal.h` fora de `motion_service`/`motion_safety` (ver item 6).

### 4. Proteção de brownout em runtime não existe — o handler é dead code
`motion_safety` subscreve `NB_EVT_POWER_BROWNOUT_WARN`, mas **ninguém publica esse evento**. `power_monitor` só detecta brownout após o reset (via `esp_reset_reason()`), como o próprio comentário admite. O requisito "brownout disable não-negociável" hoje significa apenas "safe mode no boot seguinte após N quedas".

**Correção:** dar ao power_monitor uma task periódica (Layer 3, core 1, ~10–20 Hz) lendo a tensão do barramento 5 V dos servos via ADC (divisor resistivo — verificar se `docs/ENERGY.md` já prevê o ponto de medição). Publicar `BROWNOUT_WARN` com threshold + histerese **antes** do nível de reset do chip (ex.: warn a 4,6 V). Alternativa/complemento: a leitura de Present Voltage do próprio servo (`servo_hal_read_voltage` já existe e não é usada pela safety_task) na varredura de 20 Hz. Documentar explicitamente que o BOD de hardware reseta o chip sem callback.

### 5. Race conditions na máquina de estados do motion_safety
`s_state` é escrito por três contextos (arm() na task chamadora, do_fault() na safety_task, on_brownout() na dispatcher task) e o mutex cobre só trechos do arm(). Cenário concreto: brownout/fault chega enquanto arm() está em INITIALIZING → on_brownout seta DISABLED e desliga torque → arm() prossegue e finaliza com `s_state = ARMED`. O robô fica armado logo após um evento que mandava desarmar.

**Correção:** centralizar toda transição numa única função `transition(esperado, novo)` com critical section (ou `atomic_compare_exchange`); arm() deve fazer a transição final INITIALIZING→ARMED de forma condicional — se o estado mudou no meio, abortar. on_brownout/do_fault devem invalidar arm() em andamento (flag de geração/epoch incrementada a cada fault).

### 6. O veto do safety não está no choke point
`servo_hal_write_position()` é símbolo público e não consulta o safety (Layer 1 não pode chamar Layer 3 — correto). Hoje o check é responsabilidade do chamador: motion_service faz; web_service não faz. A regra "toda escrita passa por `motion_safety_check_position()`" depende de disciplina, não de arquitetura.

**Correção (duas opções, sem violar camadas):**
(a) *Gate por injeção:* servo_hal expõe `servo_hal_set_write_guard(fn)`; motion_safety registra seu validador no init. HAL chama o guard antes de toda escrita — dependência continua apontando para baixo (HAL só conhece um ponteiro de função).
(b) *Visibilidade restrita:* separar as funções de escrita em `servo_hal_write.h` privado, incluível apenas por motion_service/motion_safety (convenção + verificação por script no build).
A opção (a) é mais robusta; (b) é mais simples. Idealmente ambas.

### 7. Latência de parada de emergência não tem orçamento garantido
`park_and_disable()` disputa o `s_bus_mutex` (timeout 50 ms) com a safety_task, que faz 4 transações UART por ciclo, cada uma com até 3 retries. Pior caso de `disable_torque` pode estourar o requisito de <150 ms para stall. Além disso, `park_and_disable` escreve posição de parking e **imediatamente** desliga torque — o parking nunca executa (torque off cancela o movimento); o comando só ocupa o bus no momento mais crítico.

**Correção:** no caminho de emergência, eliminar o parking (ou torná-lo opcional pós-análise) e enviar só torque-off, com 1 tentativa e sem retry. Medir o pior caso real com trace (esp_timer antes/depois) e documentar o orçamento em `SERVO_SAFETY.md`. Considerar broadcast torque-off (ID 0xFE, como o servo_test já faz) para desligar os dois servos numa única transação.

### 8. Interferência RF no bus dos servos tratada com "mandar 8×"
O próprio código documenta 20–30% de perda de pacotes no UART do servo (GPIO 19/20 = USB D±) com WiFi ativo, e mitiga repetindo comandos 8× fire-and-forget. Para comandos de posição isso é perigoso (réplica atrasada = movimento fantasma) e mascara um problema de hardware.

**Correção:** (1) curto prazo: substituir fire-and-forget por write + read-back de posição (verificação real de entrega); suspender movimento quando taxa de falha de leitura subir; (2) médio prazo: avaliar realocação dos pinos do servo UART (respeitando a restrição dos pinos DVP) ou mitigação física (resistor série, cabo curto/blindado, baud menor); (3) registrar a taxa de erro do bus como métrica de diagnostics para quantificar antes/depois.

---

## P1 — Arquitetura

### 9. `web_service.c` é um god-component que inverte a arquitetura em camadas
5.033 linhas em `infra/` (Layer 2) incluindo headers das Layers 1–7: servo_hal, conductor, expression, persona, long_term_memory, camera, vision, audio v1+v2… Chama para cima (`conductor_pause()`), chama HAL direto, subscreve 11 tipos de evento. É o maior arquivo do firmware e o mais acoplado.

**Correção:** reclassificá-lo como *adapter de borda* (interface externa, conceitualmente "Layer N+1" que enxerga facades) e movê-lo para `components/interface/web/`. Quebrar por domínio: `api_system`, `api_motion`, `api_audio`, `api_vision`, `api_ota`, cada um falando apenas com o serviço de domínio correspondente (nunca HAL). Comandos viram eventos/chamadas de facade; estado é lido de getters dos serviços. Meta de tamanho: nenhum módulo >800 linhas.

### 10. Watchdog é placebo
`wdog_task` (prio 24) só alimenta o TWDT em loop — nenhuma task crítica é monitorada. Se render, motion, audio ou dispatcher travarem, o TWDT continua sendo alimentado e nada acontece. `app_main` ainda chama `nb_watchdog_feed()` sem ter feito `esp_task_wdt_add()` (erro silencioso a cada 2 s).

**Correção:** padrão check-in agregado: tasks críticas (render, motion, safety, dispatcher, audio) registram-se no watchdog_service e setam um bit de heartbeat por ciclo; a wdog_task só alimenta o TWDT quando todos os bits do período chegaram — senão deixa estourar (panic + coredump). Alternativa mais simples: cada task crítica entra direto no TWDT via `nb_watchdog_add_task()` e alimenta a si mesma. Remover o feed do app_main e deletar a task após boot, como o comentário já sugere.

### 11. Eventos de safety podem ser dropados por backpressure normal
A fila de safety é separada, mas o **pool de 32 eventos é compartilhado**. Rajada de eventos normais esgota o pool → `pool_acquire()` retorna NULL → evento de SAFETY é dropado. Viola "eventos de safety nunca são bloqueados por backpressure normal".

**Correção:** reservar slots exclusivos para safety (ex.: pool particionado — últimos 4 slots só para `is_safety_event()`), contador de drops separado para safety com log em nível ERROR e, idealmente, fallback síncrono: se publish_async de safety falhar, chamar deliver_event inline (degradação controlada é melhor que perda).

### 12. `nb_event_publish` síncrono executa handlers no contexto do chamador
18 call sites usam o publish síncrono. Handlers de subscribers arbitrários rodam na task do publisher: inversão de prioridade, consumo de stack alheio, risco de reentrância em mutexes (publisher segura lock X, handler tenta X). 

**Correção:** política escrita no header: sync permitido apenas em init/boot e em contexto de prioridade ≤ dispatcher; runtime usa async. Adicionar assert de runtime (se prioridade da task chamadora > NB_EVENT_BUS_TASK_PRIORITY, logar/abortar em build de debug). Auditar os 18 usos (bridge_service e rhythm_service são os mais suspeitos por rodarem em tasks quentes).

### 13. Pipeline de áudio duplicada (v1 + v2)
`audio_service` (1.934 linhas) coexiste com 5 componentes `*_v2` e inicializa só parte deles. Dois donos potenciais do I2S, duplicação de lógica de captura/VAD, e web_service inclui as duas gerações.

**Correção:** concluir a migração: declarar a v2 canônica, reduzir audio_service a facade fina (ou eliminá-lo), apagar caminhos mortos. Enquanto coexistirem, documentar no header de cada um qual é dono de I2S0/I2S1 e em qual estado.

### 14. Código de câmera/visão ativo contradiz o hardware declarado
CLAUDE.md e a tabela de hardware dizem OV2640 **Adiado**, mas existem camera_hal (915 linhas), camera_service, vision_service, vision_preview_service compilados e endpoints de enrollment facial no dashboard. Além do drift documental, isso consome o headroom de PSRAM (>300 KB) que a própria regra manda preservar.

**Correção:** atualizar CLAUDE.md, HARDWARE.md e ROADMAP para o estado real (câmera ativa em experimento). Colocar vision/camera atrás de opção Kconfig para builds sem câmera. Adicionar verificação periódica de `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` no diagnostics com alarme abaixo de 300 KB.

### 15. Segurança de rede: credenciais no binário, HTTP aberto, OTA sem verificação aparente
`wifi_creds.h` (fora do git, ok) embute SSID/senha em texto plano no binário flashado. O HTTP não tem autenticação (nenhum endpoint checa token) e expõe OTA, reboot, calibração de servo e config. CLAUDE.md veta TLS por custo de RAM — aceitável em rede local, mas exige outra camada.

**Correção:** provisioning de WiFi via NVS (SoftAP ou BLE futuro), eliminando o header. Token de API por dispositivo (gerado no primeiro boot, em NVS, mostrado no display/serial) exigido em todo endpoint mutador. Para OTA: validar `esp_app_desc_t` (projeto/versão) e hash SHA-256 da imagem antes de `esp_ota_set_boot_partition`; planejar anti-rollback quando sair do estágio experimental.

---

## P2 — Robustez e higiene

### 16. Build de produção em `-Og` (CONFIG_COMPILER_OPTIMIZATION_DEBUG=y)
Render, áudio e DSP rodando sem otimização; medições de jitter/CPU irrealistas e ~30–50% de folga de CPU desperdiçada. **Correção:** `-O2` (PERF) no sdkconfig.defaults para builds de validação; manter DEBUG só em perfil local separado (`sdkconfig.debug.defaults`).

### 17. UART do servo: descarte de eco por `uart_flush_input` pode comer a resposta
Os comentários em `send_packet()` se contradizem (primeiro explicam por que flush é racy, depois usam flush). Se a resposta do servo chegar antes do flush executar (preempção após `uart_wait_tx_done`), ela é descartada → timeout → retry → jitter no bus. **Correção:** ler exatamente N bytes de eco com timeout curto (a abordagem descartada — investigar por que "quebrou" o ring buffer, provavelmente bug de contagem), ou desabilitar RX durante TX. Corrigir o comentário para refletir a decisão.

### 18. `power_monitor_set_mode` sem thread-safety e sem evento
Estado global mutável sem mutex; mudança de modo (NORMAL→SAFE_MODE etc.) não publica evento, violando a regra "mudou estado significativo → publica no bus". **Correção:** proteger com critical section e publicar `NB_EVT_POWER_MODE_CHANGED` (criar o tipo).

### 19. `motion_safety_emergency_stop` não publica evento
Seta FAULT e desliga torque, mas não emite `NB_EVT_MOTION_FAULT` — as camadas superiores (LED de erro, expressão, log persistente) não ficam sabendo. **Correção:** publicar o evento (assíncrono, fila safety) após o park.

### 20. Limite de 4 subscribers por tipo com 78 tipos de evento
Tipos populares (state changed, expression) tendem a estourar o limite silenciosamente — subscribe retorna `ESP_ERR_NO_MEM` com WARN, e nem todo chamador trata. **Correção:** em init de serviço, tratar falha de subscribe como fatal (NB_ASSERT). Medir ocupação real por tipo via diagnostics e subir `MAX_SUBS_PER_TYPE` (custo: 78×N×12 bytes — trivial) ou migrar para lista encadeada estática.

### 21. `servo_test.c` duplica o driver SCS inteiro
Protocolo raw reimplementado fora do servo_hal, com comportamento de flush diferente — duas verdades sobre o mesmo bus. **Correção:** quando o teste sobreviver (ver item 1), reescrevê-lo sobre o servo_hal.

### 22. Drift documental sistêmico
CLAUDE.md: estrutura de componentes não menciona web_service, bridge_service, wifi_service, diagnostics, nvs_hal, i2c_hal, camera_hal, boredom_service, voice_controller, os v2, ui_overlay, vision*; afirma "WiFi desabilitado nos Blocos 0–8" com WiFi ativo; watchdog_service comenta timeout 5 s com sdkconfig em 10 s. **Correção:** sincronizar CLAUDE.md/ARCHITECTURE.md com o código real e adotar a regra de que toda fase skipada no boot apareça no diagnostics como pendência, não só em log.

### 23. Tabela de tasks sem fonte única de verdade
Há dezenas de `xTaskCreate*` espalhados com cores/prioridades definidos localmente (safety 23, wdog 24, dispatcher 8, render 7, persistence 5…). Sem visão consolidada, é fácil criar inversões (ex.: dispatcher a 8 entrega eventos de safety para handlers — abaixo da safety_task a 23 que depende dele no caminho de brownout). **Correção:** consolidar prioridades/cores/stacks em um único header (`nb_task_config.h`) e manter a tabela em ARCHITECTURE.md; revisar especificamente a prioridade do dispatcher por estar no caminho de eventos de safety (considerar elevar, ou entregar eventos de safety por chamada direta entre componentes da Layer 3, que a arquitetura permite).

---

## Pontos positivos (manter)

Event bus com pool estático e fila de safety separada (conceito correto, falta o item 11); sprites 100% em PSRAM com double buffer; persistence_task em prio 5 com flush assíncrono de logs; idle_service implementa fielmente a regra de baseline NEUTRAL com guards por motif; HAL não publica no bus (regra respeitada); recv_response com resincronização de header bem feita; boot_manager com fases nomeadas e safe mode por contagem de brownout.

## Ordem de ataque sugerida

1. Itens 1–3 (um dia de trabalho: remover test do main, reativar PHASE_SAFETY, fechar bypass HTTP) — eliminam o risco físico imediato.
2. Itens 5–7 + 10 (transições atômicas, choke point, orçamento de e-stop, watchdog real) — tornam o safety confiável antes de reativar PHASE_MOTION.
3. Itens 4 e 8 (brownout runtime via ADC/voltage read, métrica de erro do bus) — fecham os requisitos não-negociáveis.
4. Itens 9, 13, 14 (decompor web_service, unificar áudio, gate da visão) — dívida arquitetural.
5. P2 conforme oportunidade; item 16 (otimização) antes de qualquer medição de performance.
