# NoiseBot — Revisão Estruturada de Achados (v3)

**Data:** 2026-06-11 · **Consolida e substitui para fins de execução:** `ANALISE_ARQUITETURA_2026-06-11.md` e `ANALISE_COMPLETA_E_ROADMAP_2026-06-11.md` (mantidos como contexto).
**Contexto:** servos fisicamente desconectados; PHASE_SAFETY/PHASE_MOTION skipadas de propósito.

**Campos por achado:** Evidência (arquivo/função/linha aprox.) · Regra CLAUDE.md violada/atendida · Severidade · Categoria · Forma correta · Menor correção segura · Validação.

**Severidade:** P0 = risco físico/segurança ou no caminho inevitável da reintegração · P1 = compromete confiabilidade/arquitetura · P2 = robustez/qualidade · P3 = higiene/convenção.
**Categorias:** `erro real` · `funciona mas errado` · `acerto` · `acerto evolutivo` · `inexistente`.

---

## 0. CLAUDE.md como autoridade — veredito da revisão

As análises anteriores usaram o CLAUDE.md como referência implícita; esta revisão ancora **cada achado** na regra correspondente. Três conclusões novas dessa auditoria de autoridade:

1. **O código viola regras do CLAUDE.md que não tinham sido checadas** — `-Wall -Wextra -Werror` não é aplicado pelo projeto (F27), o prefixo `nb_` é descumprido em massa (F28), e constantes de hardware aparecem hardcoded fora de `nb_hw_config.h` (F02, servo_test).
2. **O próprio CLAUDE.md está desatualizado** (F29) — e como ele declara "autoridade máxima sobre qualquer instrução geral" (CLAUDE.md, cabeçalho), um documento-autoridade errado é pior que ausente: instrui a IA e os humanos na direção errada. Corrigi-lo é meta-prioridade.
3. **Conflito de governança ativo:** CLAUDE.md (linhas 12–18) **ordena** atualizar o Knowledge OS externo a cada mudança relevante; ROADMAP.md (linha ~22) registra "Não atualizar por enquanto, por decisão do usuário". Pela hierarquia declarada, o CLAUDE.md vence — logo a decisão do usuário precisa ser escrita **no CLAUDE.md**, senão toda sessão de IA fica em estado inconsistente.

---

## 1. Achados P0

### F01 — servo_test com movimento embutido no app_main
- **Evidência:** `main/main.c:14` (include "TEMPORÁRIO"), `main/main.c:25` (`nb_servo_test_ping()` antes de `boot_manager_run()`); `main/servo_test.c:35` (`NB_SERVO_TEST_ENABLE_MOTION 1`), `:786–791` (`test_motion_servo(1/2)` se ping responder). Hardcodes `TEST_CENTER 512`/`TEST_MIN/MAX` em `servo_test.c:40–44`.
- **Regra CLAUDE.md:** "Nenhum movimento de servo é implementado antes de motion_safety estar verde" + "constantes de hardware em nb_hw_config.h — nunca hardcoded".
- **Severidade:** P0 · **Categoria:** erro real (latente — dispara no instante em que os servos forem plugados, em todo boot).
- **Forma correta:** diagnóstico de servo como componente separado, atrás de `CONFIG_NB_DIAG_SERVO` (default n), executável apenas com `motion_safety_is_armed()==true`, usando servo_hal (não protocolo duplicado), com limites vindos de `nb_hw_config.h`/config.
- **Menor correção segura:** apagar as linhas 14 e 25 de `main.c` (2 linhas; o resto do arquivo vira código morto inofensivo até a migração).
- **Validação:** build limpo; boot até COMPLETE sem tráfego no UART1 (verificar com analisador lógico ou log de TX); grep de CI garantindo que `servo_test.h` não é incluído em `main/`.

### F02 — Endpoint HTTP de calibração: bypass de safety, sem autenticação
- **Evidência:** `components/infra/web_service.c:1097–1102` (comentário "bypass de safety intencional" + justificativa de 20–30% de perda RF), `:1103–1126` (macro `CAL_SEND` chamando `servo_hal_enable_torque`/`servo_hal_write_position` 8×, clamp hardcoded `pos >= 0 && pos <= 1023` na linha ~1118 ignorando limites de config), `:1091` (`conductor_pause(true)` — Layer 2 chamando Layer 5).
- **Regra CLAUDE.md:** "Toda escrita de posição passa obrigatoriamente por motion_safety_check_position()" + "Camadas só chamam para baixo" + "comunicação entre camadas não adjacentes sempre via event bus".
- **Severidade:** P0 · **Categoria:** erro real (latente; acionável por qualquer host da LAN).
- **Forma correta:** calibração como sub-modo do estado MAINTENANCE (ver F40), via `motion_service` com perfil "calibração" (limites largos, velocidade reduzida, safety armado), autenticada por token, com entrada confirmada fisicamente e timeout (o timer de 10 s já existe — manter).
- **Menor correção segura:** envolver o handler em `#if CONFIG_NB_CALIB_HTTP` (default n) ou retornar 403 incondicional; 1 guarda de compilação.
- **Validação:** teste manual: POST de calibração retorna 403/404; nenhum símbolo `servo_hal_*` referenciado por web_service no mapa de link.

### F03 — Races nas transições de estado do motion_safety
- **Evidência:** `components/infra/motion_safety.c` — `s_state` volatile escrito por: `arm()` `:299` e `:371–373` (mutex cobre só a escrita final), `do_fault()` `:102–119` (sem mutex, comentário admite "não pode usar mutex"), `on_brownout()` `:123–140` (contexto da dispatcher task, sem mutex). Cenário: brownout durante INITIALIZING passa pelo guard `:128–131` (só checa DISABLED/FAULT), seta DISABLED; `arm()` prossegue e finaliza ARMED.
- **Regra CLAUDE.md:** "Stall detection, heartbeat timeout e brownout disable são não-negociáveis" (a race anula o disable).
- **Severidade:** P0 (pré-condição da 5.x) · **Categoria:** erro real.
- **Forma correta:** função única `transition(expected, new)` com `taskENTER_CRITICAL` ou `atomic_compare_exchange`; `arm()` faz INITIALIZING→ARMED condicional (falha se o estado mudou); contador de época (epoch) incrementado em todo fault/brownout, conferido por `arm()` antes de finalizar.
- **Menor correção segura:** no final de `arm()` (`:371`), dentro do mutex, verificar `s_state == NB_MOTION_INITIALIZING` antes de setar ARMED; abortar caso contrário.
- **Validação:** teste de concorrência em build host (D43): injetar fault/brownout em todos os pontos de interleaving do arm(); assert de que ARMED nunca sucede um fault não tratado.

### F04 — Proteção de brownout em runtime não existe (handler é dead code)
- **Evidência:** `components/infra/power_monitor.c:97–104` (comentário admitindo que não há callback de brownout em runtime; detecção só via `esp_reset_reason()` pós-reset `:89`); ninguém no firmware publica `NB_EVT_POWER_BROWNOUT_WARN` (grep vazio fora de motion_safety, que apenas subscreve em `motion_safety.c:251–253`).
- **Regra CLAUDE.md:** "brownout disable é não-negociável".
- **Severidade:** P0 (pré-condição da 5.x) · **Categoria:** inexistente (o evento e o handler existem; o produtor não).
- **Forma correta:** task periódica no power_monitor (Layer 3, core 1, 10–20 Hz) amostrando o 5 V dos servos via ADC (divisor resistivo) com threshold+histerese acima do nível de reset do chip; publicar WARN na fila de safety. Complemento barato: incluir `servo_hal_read_voltage()` (existe e está sem uso, `servo_hal.c:414–420`) na varredura da safety_task.
- **Menor correção segura:** adicionar leitura de Present Voltage à safety_task (`motion_safety.c:160–229`) com fault por subtensão — só software, sem hardware novo.
- **Validação:** bancada com fonte ajustável: derrubar tensão gradualmente e confirmar torque-off antes do reset; registrar latência medida em `SERVO_SAFETY.md`.

### F05 — Caminho de emergência sem orçamento de latência e com parking contraditório
- **Evidência:** `motion_safety.c:82–94` — `park_and_disable()` envia posição (400 ms) e desliga torque imediatamente (parking nunca executa); disputa `s_bus_mutex` com timeout de 50 ms (`servo_hal.c:55,60`) contra a própria safety_task que faz 4 transações/ciclo com até 3 retries (`servo_hal.c:340–372`).
- **Regra CLAUDE.md:** requisito de <150 ms para stall citado no próprio código (`motion_safety.c:78–79`).
- **Severidade:** P0 (pré-condição da 5.x) · **Categoria:** funciona mas errado.
- **Forma correta:** caminho de emergência dedicado: broadcast torque-off (ID 0xFE — servo_test já usa em `scs_broadcast_torque_disable`) numa única transação, 1 tentativa, sem retry, sem parking; flag que aborta retries em andamento no servo_hal.
- **Menor correção segura:** remover as duas chamadas `servo_hal_write_position` de `park_and_disable()` (`:88–89`) — só torque-off.
- **Validação:** medir com `esp_timer` o intervalo fault→último byte do torque-off no pior caso (safety_task no meio de retry); p99 < 150 ms documentado.

---

## 2. Achados P1

### F06 — Fases de boot skipadas silenciosamente
- **Evidência:** `boot_manager.c:1129` (`phase_skip(SAFETY, "motion desativado temporariamente")` seguido de `phase_ok` na `:1130` — a fase consta como OK), `:1411–1414` (PHASE_MOTION idem).
- **Regra CLAUDE.md:** "Ao mudar estado significativo: publicar evento" (estado de capability do robô é significativo).
- **Severidade:** P1 · **Categoria:** funciona mas errado.
- **Forma correta:** skip de fase publica evento e registra pendência no diagnostics; status rail (16.2) exibe capability degradada. `phase_skip` e `phase_ok` mutuamente exclusivos.
- **Menor correção segura:** remover o `phase_ok` da linha 1130 e logar em nível W com lista de fases skipadas no fim do boot.
- **Validação:** boot log mostra resumo "fases skipadas: SAFETY, MOTION"; diagnostics snapshot lista capabilities.

### F07 — Veto do safety fora do choke point
- **Evidência:** `servo_hal.c:430–453` (`servo_hal_write_position` pública, sem consulta a safety); o check vive apenas no chamador `motion_service.c:260–266` (`s_iface.check_position`). web_service não checa (F02).
- **Regra CLAUDE.md:** "Toda escrita de posição passa obrigatoriamente por motion_safety_check_position()".
- **Severidade:** P1 (P0 quando servos voltarem) · **Categoria:** funciona mas errado (regra por disciplina, não por arquitetura).
- **Forma correta:** write-guard injetado — `servo_hal_set_write_guard(fn)` registrado pelo motion_safety no init; HAL recusa escrita se o guard negar (dependência continua apontando para baixo). Complementar com header privado `servo_hal_write.h`.
- **Menor correção segura:** mover os protótipos de escrita para header separado incluído apenas por motion_service/motion_safety + verificação grep no build.
- **Validação:** teste host: escrita com guard negando retorna erro; grep de CI: nenhum include do header privado fora dos dois componentes.

### F08 — EMI no bus do servo mitigada estatisticamente
- **Evidência:** `web_service.c:1097–1101` (comentário: "GPIO 20 (TX) sofre ~20-30% perda de pacotes com WiFi ativo (USB D- RF)... P(todas 8 falharem) < 0.001%").
- **Regra CLAUDE.md:** "Pinos DVP da câmera... Nunca realocar esses GPIOs" (restringe alternativas — decisão de pinos é de hardware).
- **Severidade:** P1 (bloqueador de hardware da 5.x) · **Categoria:** funciona mas errado.
- **Forma correta:** protocolo write+read-back (confirmar posição-alvo após escrita); métrica de taxa de erro do bus no diagnostics; mitigação física avaliada (pino alternativo fora dos DVP, resistor série 100–470 Ω, cabo curto/blindado, baud reduzido) antes de movimento real.
- **Menor correção segura:** instrumentar contador de timeouts/erros por transação no servo_hal e expor no diagnostics — medir antes de decidir.
- **Validação:** taxa de erro com WiFi ativo + tráfego < 1% sustentado por 1 h antes do gate da 5.x (F44).

### F09 — web_service: god-component que inverte as camadas
- **Evidência:** `components/infra/web_service.c` — 5.033 linhas; includes das Layers 1–7 em `:2–57` (servo_hal, conductor, persona, long_term_memory, camera/vision, audio v1+v2...); `conductor_pause()` `:1091`; 11 subscriptions (grep `nb_event_subscribe` = 11).
- **Regra CLAUDE.md:** "Camadas só chamam para baixo"; estrutura de componentes não prevê web_service em infra.
- **Severidade:** P1 · **Categoria:** funciona mas errado.
- **Forma correta:** adapter de borda em `components/interface/web/`, decomposto por domínio (api_system, api_motion, api_audio, api_vision, api_ota), falando apenas com facades de serviço e event bus; zero includes de HAL.
- **Menor correção segura:** extrair só o OTA (`:1205–1276`) para `components/interface/web_ota/` — é o bloco mais isolado; prova o padrão sem big-bang.
- **Validação:** build + smoke test dos endpoints após cada extração; mapa de includes do web_service só pode encolher (script de CI).

### F10 — Pool único do event bus permite drop de evento de safety
- **Evidência:** `event_bus.c:259–264` (`pool_acquire()` NULL → evento descartado, inclusive safety), pool compartilhado de 32 (`event_bus.h:34`); filas separadas só depois do pool (`:268`).
- **Regra CLAUDE.md:** "Eventos de safety... nunca são bloqueados por backpressure normal".
- **Severidade:** P1 · **Categoria:** funciona mas errado.
- **Forma correta:** partição do pool (N slots exclusivos para `is_safety_event()`); fallback: se publish_async de safety falhar, entrega síncrona inline; contador de drop separado com log ERROR.
- **Menor correção segura:** em `pool_acquire`, reservar os últimos 4 slots para safety (um parâmetro bool); ~10 linhas.
- **Validação:** teste host: saturar fila normal com 32+ eventos e confirmar que publish de safety ainda sucede.

### F11 — Watchdog placebo
- **Evidência:** `watchdog_service.c:24–48` (wdog_task prio 24 só alimenta TWDT incondicionalmente); `main.c:43` (`nb_watchdog_feed()` sem `esp_task_wdt_add` — `esp_task_wdt_reset` retorna erro ignorado, `watchdog_service.c:97–100`); comentário `:61` diz TIMEOUT_S=5, sdkconfig.defaults define 10.
- **Regra CLAUDE.md:** (espírito de) watchdog na Layer 2 como componente de confiabilidade.
- **Severidade:** P1 · **Categoria:** funciona mas errado (TWDT dispara apenas se a própria wdog_task morrer).
- **Forma correta:** check-in agregado: tasks críticas registram-se e setam bit por ciclo; wdog_task alimenta TWDT apenas com todos os bits presentes; integração com health_score do diagnostics.
- **Menor correção segura:** registrar render_task e audio_task diretamente no TWDT via `nb_watchdog_add_task()` (API já existe) e alimentar dentro dos seus loops; remover o feed do `main.c`.
- **Validação:** teste destrutivo em bancada: suspender render_task via debugger/loop infinito → panic + coredump em ≤ timeout.

### F12 — Superfície de rede sem autenticação + credenciais no binário
- **Evidência:** `components/infra/wifi_creds.h:5–6` (SSID/senha em texto plano, compilados na imagem; fora do git, mas dentro de todo .bin distribuído); zero verificação de token/senha nos handlers HTTP (grep "auth/token" vazio em web_service.c); endpoints de reboot/OTA/config/calibração abertos.
- **Regra CLAUDE.md:** "Sem TLS/HTTPS no firmware" (aceito) — mas não autoriza ausência de qualquer autenticação.
- **Severidade:** P1 · **Categoria:** erro real (segurança).
- **Forma correta:** token por dispositivo gerado no primeiro boot (NVS, exibido no display), header obrigatório em todo método mutador; WiFi provisionado via NVS/SoftAP (F46), eliminando wifi_creds.h.
- **Menor correção segura:** middleware único de checagem de token aplicado aos POSTs (os GETs de status podem ficar abertos); ~40 linhas num só lugar.
- **Validação:** POST sem token → 401; com token → 200; varredura da LAN (nmap + curl) confirmando que nenhum mutador responde sem token.

### F13 — Publish síncrono sem política de uso
- **Evidência:** `event_bus.c:248–253` (`nb_event_publish` executa `deliver_event` no contexto do chamador); 18 call sites (boot_manager, bridge_service, diagnostics, circadian, rhythm, touch_semantic, vad_semantic).
- **Regra CLAUDE.md:** "Nunca chamar subscriber diretamente para comunicação cross-layer" — o publish sync é exatamente isso com outro nome.
- **Severidade:** P1 · **Categoria:** funciona mas errado.
- **Forma correta:** política no header: sync apenas em init/boot; runtime sempre async; assert em build de debug se task de prioridade > dispatcher usar sync.
- **Menor correção segura:** converter os call sites de bridge_service e rhythm_service (tasks quentes) para `publish_async`; documentar o resto.
- **Validação:** revisão dos 18 sites com classificação (boot-ok / convertido); teste de regressão dos fluxos afetados.

### F14 — Pipeline de áudio duplicada (v1+v2)
- **Evidência:** `audio_service.c:1617–1626` (v1 inicializa playback_v2 e voice_activity_v2 por dentro); 5 componentes `*_v2` coexistindo com audio_service de 1.934 linhas; web_service inclui as duas gerações (`web_service.c:40–46`).
- **Regra CLAUDE.md:** (princípio de camadas/ownership único de HAL por serviço Layer 4).
- **Severidade:** P1 · **Categoria:** funciona mas errado.
- **Forma correta:** v2 canônico; audio_service reduzido a facade de compatibilidade; documentação de ownership de I2S0/I2S1 por header; deleção dos caminhos mortos.
- **Menor correção segura:** documentar no topo de cada componente quem é dono de qual periférico e qual geração é canônica (só comentários — zero risco).
- **Validação:** wake→captura→playback funcionando após cada remoção; nenhum init duplo de I2S no boot log.

---

## 3. Achados P2

### F15 — config_manager sem cache RAM
- **Evidência:** `config_manager.c:302–320` — getters fazem `nvs_hal_get_i16` direto (flash) a cada chamada; sem shadow struct (grep `s_cache` vazio). Chamado por `motion_safety_check_position()` (`motion_safety.c:389–390`) — no futuro, a 50 Hz.
- **Severidade:** P2 (P1 quando motion voltar) · **Categoria:** funciona mas errado.
- **Forma correta:** shadow struct em RAM no init; set escreve NVS+RAM e publica `NB_EVT_CONFIG_CHANGED`; versionamento de schema.
- **Menor correção segura:** cachear apenas os 6 valores de servo (min/max/center ×2) em statics carregados no init e invalidados nos setters.
- **Validação:** teste host dos getters/setters; medir tempo de `check_position` antes/depois (alvo: <10 µs).

### F16 — Build de validação em -Og
- **Evidência:** `sdkconfig` ativo: `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y`.
- **Severidade:** P2 · **Categoria:** funciona mas errado.
- **Forma correta:** `sdkconfig.defaults` com `CONFIG_COMPILER_OPTIMIZATION_PERF` (-O2); perfil debug separado; medições de performance sempre citando o perfil.
- **Menor correção segura:** trocar a flag e recompilar (atenção ao workaround `-fno-ipa-sra` já presente para o ICE conhecido — `CMakeLists.txt:17–20`).
- **Validação:** build -O2 sem warnings novos; FPS/latência de áudio re-medidos; soak test de 24 h.

### F17 — Coredump para UART em produto headless
- **Evidência:** `sdkconfig`: `CONFIG_ESP_COREDUMP_ENABLE_TO_UART=y`; `partitions.csv` sem partição coredump.
- **Severidade:** P2 · **Categoria:** funciona mas errado.
- **Forma correta:** coredump em flash (partição dedicada ~64 KB) + envio ao server via bridge no boot seguinte + snapshot de diag junto (caixa-preta, F47).
- **Menor correção segura:** adicionar partição `coredump` reduzindo `storage` em 64 KB e trocar a flag — exige reflash da tabela (avisar: apaga SPIFFS).
- **Validação:** provocar panic controlado; confirmar dump recuperável via `espcoredump.py` no boot seguinte.

### F18 — OTA sem rollback automático
- **Evidência:** `web_service.c:1205–1276` — `esp_ota_begin/write/end/set_boot_partition` sem `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, sem validação de `esp_app_desc_t.project_name`, sem mark_valid pós-boot.
- **Severidade:** P2 · **Categoria:** acerto evolutivo (funciona; falta rede de segurança).
- **Forma correta:** rollback habilitado; `esp_ota_mark_app_valid_cancel_rollback()` só após health check (boot COMPLETE + N s sem panic); validação de project_name/versão antes de gravar.
- **Menor correção segura:** validar `project_name == "noisebot"` do header da imagem antes do `esp_ota_write` do primeiro bloco.
- **Validação:** OTA de imagem corrompida → rejeitada; OTA de imagem que entra em panic-loop → rollback automático para a anterior.

### F19 — Flush de log para SD é placeholder
- **Evidência:** `logger.c:53–68` — comentário descreve o design (ring buffer PSRAM + flush via persistence 60 s) e declara o callback como "placeholder"; Etapa 0.3 não implementada.
- **Severidade:** P2 · **Categoria:** inexistente.
- **Forma correta:** a do próprio comentário (vprintf hook → ring buffer PSRAM → persistence_task) + flush forçado em crash.
- **Menor correção segura:** implementar exatamente o descrito, começando só com nível W/E (volume baixo).
- **Validação:** logs aparecem em `/sdcard/logs/` após 60 s; nenhuma escrita SD em task prio ≥10 (conferir por inspeção da fila da persistence).

### F20 — Dimming por software no framebuffer inteiro
- **Evidência:** `render_service.cpp:152–161` — loop por pixel decompõe/recompõe RGB565 a cada frame para brilho global.
- **Severidade:** P2 · **Categoria:** acerto evolutivo.
- **Forma correta:** brilho global via PWM (LEDC) no pino de backlight; per-pixel reservado a efeitos locais.
- **Menor correção segura:** curto-circuito quando `level == 255` (pular o loop inteiro) — 2 linhas, ganho imediato no caso comum.
- **Validação:** FPS medido antes/depois (provider `get_fps` do diagnostics, F32); ausência de flicker visível no PWM (>1 kHz).

### F21 — Acoplamento frágil com ESP-SR via extern manual
- **Evidência:** `audio_service.c:41–48` — protótipos `extern` de `vad_create_with_param`/`vad_process` redeclarados à mão para evitar conflito de headers.
- **Severidade:** P2 · **Categoria:** funciona mas errado (UB silencioso se a ABI do componente mudar).
- **Forma correta:** wrapper `nb_esp_sr_compat.c` que inclui o header real em TU isolado e exporta API própria; assert de versão do componente gerenciado.
- **Menor correção segura:** mover os externs para um .c isolado que inclua `esp_vad.h` de verdade e resolva o conflito de nomes com `#define` local.
- **Validação:** build após `idf.py update-dependencies`; teste de wake/VAD em bancada.

### F22 — conductor_pause é bool global sem dono
- **Evidência:** `conductor.c:429–432`; chamado por web_service na calibração (`web_service.c:1091`).
- **Severidade:** P2 · **Categoria:** funciona mas errado.
- **Forma correta:** refcount ou token de dono (pause(owner)/resume(owner)); timeout de segurança.
- **Menor correção segura:** contador atômico em vez de bool (pause incrementa, resume decrementa, ativo se >0).
- **Validação:** teste host: dois donos pausando/despausando em ordens diferentes.

### F23 — power_monitor_set_mode sem proteção nem evento
- **Evidência:** `power_monitor.c:117–130` — escrita de `s_mode` sem mutex/critical section, sem publicar evento.
- **Regra CLAUDE.md:** "Ao mudar estado significativo: publicar evento no event bus".
- **Severidade:** P2 · **Categoria:** funciona mas errado.
- **Forma correta:** transição em critical section + `NB_EVT_POWER_MODE_CHANGED` (novo tipo).
- **Menor correção segura:** critical section em volta da leitura-comparação-escrita; 4 linhas.
- **Validação:** teste host de concorrência; subscribers recebem o evento.

### F24 — emergency_stop não publica evento de fault
- **Evidência:** `motion_safety.c:433–448` — seta FAULT e parqueia sem `nb_event_publish_async(NB_EVT_MOTION_FAULT)` (diferente de `do_fault()` `:115–116`, que publica).
- **Severidade:** P2 · **Categoria:** funciona mas errado.
- **Forma correta/menor correção:** publicar o mesmo evento do do_fault (2 linhas); idealmente unificar com a função `transition()` de F03.
- **Validação:** teste host: emergency_stop gera evento na fila de safety.

### F25 — Falha de subscribe tratada como aviso ignorável
- **Evidência:** `event_bus.c:225–227` (retorna NO_MEM com LOGW); `behavior_engine.c:778,806` e outros assinam sem tratar erro como fatal; limite de 4 subs/tipo (`event_bus.h:37`) com 78 tipos (`nb_events.h`).
- **Severidade:** P2 · **Categoria:** funciona mas errado (perda silenciosa de comportamento).
- **Forma correta:** NB_ASSERT_FATAL em todo subscribe de init; diagnostics expõe ocupação de slots por tipo.
- **Menor correção segura:** checar retorno nos inits dos serviços principais e logar em E com nome do tipo.
- **Validação:** teste host: 5º subscriber no mesmo tipo dispara o assert.

### F26 — Tabela de tasks dispersa, prioridade do dispatcher questionável
- **Evidência:** dezenas de `xTaskCreate*` espalhados; dispatcher a prio 8 (`event_bus.h:39`) entrega eventos de safety consumidos por componentes que dependem dele, enquanto safety_task roda a 23 (`motion_safety.c:263`) e wdog a 24 (`watchdog_service.c:16`).
- **Severidade:** P2 · **Categoria:** inexistente (fonte única) / funciona mas errado (prioridade).
- **Forma correta:** `nb_task_config.h` com todas as constantes + tabela em ARCHITECTURE.md + verificação de build; análise dedicada da prioridade do dispatcher (ou entrega de eventos de safety por chamada direta intra-Layer-3, permitida pela arquitetura).
- **Menor correção segura:** criar o header e migrar as constantes sem alterar valores (refactor mecânico).
- **Validação:** build idêntico (diff de binário ou de mapa); revisão da tabela em par.

---

## 4. Achados P3 (conformidade com CLAUDE.md)

### F27 — `-Wall -Wextra -Werror` não é aplicado pelo projeto
- **Evidência:** `CMakeLists.txt:20` — único COMPILE_OPTIONS adicionado é `-fno-ipa-sra`; nenhum CMakeLists de componente adiciona os flags. O projeto depende dos defaults do ESP-IDF (`-Wall -Werror=all`), que **não** transformam warnings de `-Wextra` em erro.
- **Regra CLAUDE.md:** "Compilar com -Wall -Wextra -Werror — zero warnings tolerados".
- **Severidade:** P3 · **Categoria:** erro real (regra declarada e não implementada).
- **Forma correta:** `idf_build_set_property(COMPILE_OPTIONS "-Wall;-Wextra;-Werror" APPEND)` no CMakeLists raiz, com exceções pontuais por componente externo (LovyanGFX/managed) via `set_source_files_properties`.
- **Menor correção segura:** aplicar primeiro só aos componentes próprios (infra, nb_hal, services, behavior, persona) via `target_compile_options` por componente.
- **Validação:** build completo zero-warning; CI falhando em warning novo.

### F28 — Prefixo `nb_` descumprido em massa nas APIs públicas
- **Evidência:** `config_manager.h:42–78` (`config_get_*`/`config_set_*`), `servo_hal.h` (`servo_hal_*`), `conductor.h` (`conductor_*`), `audio_service.h` (`audio_*`), `motion_safety.h` (`motion_safety_*`) — nenhum com prefixo `nb_`.
- **Regra CLAUDE.md:** "Prefixo nb_ para todos os tipos, funções e macros públicas do projeto".
- **Severidade:** P3 · **Categoria:** funciona mas errado (ou: regra do CLAUDE.md irreal — decidir).
- **Forma correta:** decidir e registrar: (a) migração gradual com aliases `#define`, ou (b) emendar o CLAUDE.md para "prefixo nb_ apenas em tipos/eventos/macros globais" (refletindo a prática real). A pior opção é manter regra e prática divergentes.
- **Menor correção segura:** emendar o CLAUDE.md (1 linha) — zero risco de regressão.
- **Validação:** revisão do texto; se migração: build + grep de símbolos antigos.

### F29 — CLAUDE.md (autoridade máxima) desatualizado e em conflito com o ROADMAP
- **Evidência:** CLAUDE.md — tabela de componentes sem web_service, bridge, wifi, vision*, v2, boredom, voice_controller, ui_overlay, diagnostics, i2c_hal, camera_hal; "WiFi desabilitado nos Blocos 0–8" com WiFi em produção; câmera "Adiado" com stack de visão FEITA (ROADMAP "Feito Consolidado"); CLAUDE.md:12–18 ordena atualizar Knowledge OS vs ROADMAP.md:~22 "não atualizar por decisão do usuário".
- **Regra CLAUDE.md:** "Este arquivo tem autoridade máxima" — autoridade errada propaga erro.
- **Severidade:** P3 (mas meta-prioridade: barato e multiplicador) · **Categoria:** erro real documental.
- **Forma correta:** sincronizar CLAUDE.md com o estado real (componentes, WiFi, câmera, status de motion) e internalizar nele a decisão sobre o Knowledge OS; incluir no escopo da etapa D.1.
- **Menor correção segura:** patch só nas 4 divergências factuais listadas.
- **Validação:** releitura cruzada CLAUDE.md × árvore de componentes × ROADMAP por outra sessão de IA (teste prático de autoridade).

### F30 — Higiene menor
- **Evidência:** `watchdog_service.c:60–62` (comentário TIMEOUT_S=5 vs sdkconfig 10); `servo_hal.c:115–126` (dois blocos de comentário contraditórios sobre eco/flush — o primeiro explica por que flush é racy, o segundo manda usar flush); `main.c:31` (comentário sobre falha "nunca deveria acontecer" sem ação).
- **Severidade:** P3 · **Categoria:** funciona mas errado.
- **Correção mínima:** passada única de comentários; em servo_hal, decidir e documentar a abordagem definitiva do eco (ligado a F08).
- **Validação:** revisão de diff.

---

## 5. Acertos (manter — com evidência)

| ID | Acerto | Evidência | Regra CLAUDE.md atendida |
|----|--------|-----------|--------------------------|
| A1 | Event bus estático, fila safety drenada com prioridade, tabela copiada antes dos handlers | `event_bus.c:97–156` | "Estruturas estáticas para event bus" |
| A2 | Baseline IDLE: nenhum motif fora de NEUTRAL; entrada em IDLE limpa overlays | `idle_service.c:268, 536, 621` | "Toda entrada em IDLE deve limpar expressão…" |
| A3 | Conductor: partituras ROM declarativas, anti-repeat, interrupção em chunks 20 ms, wrap de uptime tratado | `conductor.c:75–233, 283–294` | — |
| A4 | Cooldown anti-loop físico servo→vibração→VAD documentado | `behavior_engine.c:60–66` | — (consciência de robótica acima da média) |
| A5 | Sprites/framebuffers 100% PSRAM, double buffer + push task | `render_service.cpp:355–356`, `display_hal.cpp:83` | "Nenhum framebuffer de display em SRAM" |
| A6 | Áudio: callback no caminho crítico (decisão documentada), VAD por score com histerese | `audio_service.c:1–14, 64–70` | "Nenhum malloc em caminho crítico" (preserva) |
| A7 | Bridge: framing SOF/LEN/CRC8, keep-alive, offline-first real | `bridge_service.h:8–31` | "Offline-first… conectividade é conveniência" |
| A8 | Persistência assíncrona em prio 5 | `persistence_mgr.c:31` | "Nunca SD síncrono em task ≥10" |
| A9 | servo_hal: endianness confirmada empiricamente e datada; resync de header no RX | `servo_hal.c:374–389, 180–198` | — |
| A10 | HAL não publica no event bus (grep limpo em nb_hal/) | — | "Nenhum HAL publica no event bus" |
| A11 | ROADMAP.md como painel vivo com critérios de saída | `docs/ROADMAP.md:9–35` | — |

**Acertos evolutivos** (corretos hoje, com evolução já especificada): F18 OTA, F20 dimming, A3→partituras carregáveis de SD em dev (F45), B7-thresholds de wake para NVS (precedente: calibração do touch 2.2A).

---

## 6. Inexistentes — o que criar (com os mesmos campos)

| ID | Item | Severidade | Forma recomendada | Menor passo seguro | Validação |
|----|------|-----------|-------------------|--------------------|-----------|
| F40 | Modo MAINTENANCE na state_machine | P1 | Estado formal agrupando calibração/OTA/diag; entrada por toque longo+token, saída por timeout; base visual própria (permitido pela regra de estados) | Adicionar o estado e migrar só a calibração para dentro dele | Transições testadas em host; calibração inacessível fora do modo |
| F41 | Supervisor de saúde (Layer 3) | P1 | Agrega check-ins de tasks, heap/PSRAM, drops do bus, erros do servo bus; publica HEALTH_DEGRADED/RECOVERED; alimenta status rail 16.2 | Componente que só lê métricas existentes (diagnostics) e publica evento | Evento dispara ao simular task travada/heap baixo |
| F42 | Monitor da regra dos 300 KB de PSRAM | P2 | Verificação periódica no supervisor; abaixo do limiar → evento + degradar features opcionais (preview de câmera primeiro) | Log W periódico quando <300 KB | Alocar bloco de teste e ver o evento disparar |
| F43 | Testes de firmware em host + CI | P1 | Build Linux com stubs FreeRTOS para state_machine, behavior rules, emotion_model, event_bus, conductor, parse SCS; CI por commit | Testar só o parse de pacote do servo_hal (função pura) | Suite roda em CI; cobre os cenários de F03/F10/F22 |
| F44 | Gate executável de reintegração de motion | P0 (pré-5.x) | Checklist: F03 corrigido+testado, F08 <1% erro, F05 <150 ms medido, F01 removido, F02 atrás de F40 | Escrever o checklist em SERVO_SAFETY.md com campos de medição | Os 5 itens verdes antes de conectar servo |
| F45 | Partituras do conductor carregáveis de SD (dev) | P3 | Loader JSON com fallback ROM; iteração de animação sem reflash | Loader read-only de 1 ação atrás de flag de build | Partitura editada no SD altera animação sem reflash |
| F46 | Provisioning WiFi + identidade | P2 | SoftAP primeiro boot (ou `/sdcard/config/wifi.json` consumido e apagado) → NVS; token de API gerado e exibido | Ler credenciais de NVS com fallback ao header atual | Boot sem wifi_creds.h compilado conecta após provisionar |
| F47 | Caixa-preta (coredump flash + snapshot diag + últimos N eventos) | P2 | Combina F17+F19+ring de eventos; upload ao server no boot pós-crash | Ring buffer dos últimos 64 eventos do bus em RAM, despejado no log em panic handler | Panic provocado gera artefato com contexto |
| F48 | Gesto universal "para tudo" | P2 | Toque longo → conductor interrupt + audio stop + NEUTRAL + (futuro) motion freeze | Mapear gesto existente do touch_semantic para conductor_play interrupt + audio_play_stop | Robô em ação qualquer cessa em <300 ms ao gesto |
| F49 | Instrumentação de energia (ADC 5 V) | P2 | Mesmo hardware serve F04 (brownout) e ENERGY.md (orçamento medido) | Especificar o divisor e o pino em HARDWARE.md antes de montar | Leitura bate com multímetro ±5% |

---

## 7. Ordem de execução — estado atual

| Rodada | Itens | Status |
|--------|-------|--------|
| R0 | F01, F02, F12-mínimo, F24, F23, F30 | ✓ FEITO (`3f2000e`) |
| P0 atual | 14.1, 16.2, D.1 (incl. F29) | em andamento no roadmap |
| R1 | F11, F15, F10, F25, F19, F17, F16, F26, F41, F42, F27 | ✓ FEITO (`e56ca1f` + código pré-existente) |
| R2 | F03, F04, F05, F07 | ✓ FEITO (sessão 2026-06-11) |
| R2 | F08, F44, F40 | ✓ FEITO (sessão 2026-06-11) |
| R3 | F09, F14, F18, F21, F46 | ✓ FEITO (sessão 2026-06-11) |
| R4 | F43, F45, F47, F48, F49, F13, F20, F22, F28 | ✓ FEITO (sessão 2026-06-11) |
