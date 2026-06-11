# NoiseBot — Análise Completa e Roadmap Revisado

**Data:** 2026-06-11 · **Complementa:** `ANALISE_ARQUITETURA_2026-06-11.md` (achados de safety detalhados lá)
**Contexto assumido:** servos/motion **em standby** — não conectados, PHASE_SAFETY/PHASE_MOTION skipadas de propósito. O sistema vivo hoje é: display/render + expressões + idle + touch + voz/wake/bridge + câmera/visão via server + persona + LED, com agenda (14.1) e status rail (16.2) em curso.

Estrutura: **(A) Correto** · **(B) Correto mas melhorar** · **(C) Errado** · **(D) Não existe e deveria ser criado** · **(E) Roadmap revisado**.

---

## A. O que está CORRETO — manter como está

**Arquitetura em camadas com event bus.** A regra "HAL nunca publica no bus" é cumprida (verificado: zero publishes em `nb_hal/`). Serviços comunicam-se por eventos ou chamadas para baixo. A exceção é o web_service (ver C5).

**Event bus.** Pool estático sem malloc, fila de safety separada com drenagem prioritária no dispatcher, cópia da tabela de subscribers antes de chamar handlers (evita deadlock e tolera unsubscribe em handler), contadores de drop com log throttled. Design correto para embarcado.

**Render e display.** Double buffer 100% em PSRAM (`setPsram(true)` verificado em display_hal e render_service), push task separada da render task, regra "nenhum framebuffer em SRAM" cumprida. C++ confinado ao display, como manda o CLAUDE.md.

**idle_service.** Implementa fielmente a regra de baseline: nenhum motif inicia fora de NEUTRAL, entrada em IDLE limpa overlay/rotação/gaze, motifs compostos com guards. É o componente mais alinhado à filosofia do produto.

**conductor.** Partituras declarativas estáticas (ROM), variações com anti-repeat via RNG de hardware, mecanismo de interrupção limpo (chunks de 20 ms com flag), critical sections corretas para estado compartilhado, tratamento de wrap de uptime. Micro-expressões antes da ação principal mostram maturidade de animação.

**behavior_engine.** Tabela declarativa (trigger, condition) → actions com precedência bem definida. Destaque raro em robótica amadora: o cooldown VOICE_ACT (4 s) documenta e previne o loop físico servo→vibração→VAD→ação→servo. Esse tipo de consciência de acoplamento físico é exatamente o que um robô de mesa precisa.

**Pipeline de voz.** Decisão consciente e documentada de usar callback em vez de event bus no caminho crítico de áudio (16 ms/chunk). VAD com score/histerese (strong/soft/decay) em vez de threshold binário. Wake com ganho adaptativo anti-clipping. Heurística local mantida só para bancada via flag de build.

**bridge_service.** Framing binário com SOF/LEN/CRC8, keep-alive, reconexão indefinida, offline-first real (robô opera sem bridge). Transporte duplo TCP/UART CDC. Contrato de mensagens versionado por enum.

**servo_hal (para quando voltar).** Protocolo com checksum validado, resincronização de header no RX, retries, mutex de bus com timeout, endianness confirmada empiricamente e documentada com data. Boa base — os problemas estão acima dele (ver C1–C3).

**boot_manager.** Fases nomeadas com enter/ok/skip, safe mode por contagem de brownout persistida em NVS, degradação SD_DEGRADED. 

**Persistência.** persistence_task em prioridade 5 com fila assíncrona — regra "nunca SD síncrono em task ≥10" respeitada na estrutura.

**Processo.** O ROADMAP.md como painel vivo com critérios de saída e a tabela "hardware que não deve guiar trabalho agora" é prática de engenharia acima da média. Manter.

---

## B. CORRETO mas precisa de melhoria — com sugestão de evolução

### B1. config_manager sem cache em RAM
Toda chamada `config_get_*` faz leitura NVS (flash). Hoje o custo é tolerável; quando motion voltar, limites de servo serão lidos a 50 Hz no caminho de safety — jitter e desgaste desnecessários.
**Melhoria:** shadow struct em RAM carregada no init; `config_set_*` escreve NVS + RAM e publica `NB_EVT_CONFIG_CHANGED`. NVS vira só persistência.
**Evolução:** versionamento de schema de config (campo `cfg_version` em NVS) com migração automática — vocês já fizeram migração ad-hoc no touch (legados 5/8/10/15/25); formalizar o mecanismo.

### B2. Watchdog existe mas não protege ninguém
`wdog_task` (prio 24) só alimenta o TWDT em loop; nenhuma task crítica é monitorada — se render, audio ou dispatcher travarem, nada dispara. `app_main` ainda chama `nb_watchdog_feed()` sem `esp_task_wdt_add()` (erro silencioso a cada 2 s).
**Melhoria:** padrão check-in agregado — tasks críticas (render, audio, dispatcher, touch, futura motion) registram-se e setam bit de heartbeat por ciclo; wdog_task só alimenta o TWDT quando todos os bits do período chegaram.
**Evolução:** integrar com o `health_score` que o diagnostics_service já calcula e com o status rail (16.2): task atrasada → ícone de degradação antes do panic.

### B3. Event bus — três refinamentos
(1) Pool de 32 compartilhado entre normal e safety: rajada normal pode esgotar o pool e dropar evento de safety (contradiz a regra declarada). Reservar slots exclusivos para safety + fallback de entrega síncrona se o pool falhar.
(2) Falha de `nb_event_subscribe` em init de serviço deve ser fatal (NB_ASSERT), não WARN ignorável — com 78 tipos e 4 slots/tipo, estouro silencioso é questão de tempo.
(3) `nb_event_publish` síncrono (18 call sites) executa handlers no contexto do chamador. Escrever a política no header (sync só em init/boot) e auditar os 18 usos.
**Evolução:** telemetria do bus no diagnostics — profundidade máxima de fila, eventos/s por tipo, top droppers. Barato e dá visibilidade que hoje não existe.

### B4. Render — dimming por software queima CPU
O brightness é aplicado pixel a pixel em software no framebuffer inteiro a cada frame (~57k pixels), agravado pelo build em -Og.
**Melhoria:** mover dimming global para PWM no pino de backlight (LEDC); manter o per-pixel só para efeitos localizados (vinheta, fade de overlay).
**Evolução:** publicar FPS real no diagnostics (o provider `get_fps` já existe na interface — ligar) e definir orçamento de frame documentado (ex.: ≤ 25 ms p95).

### B5. Coredump vai para UART — caixa-preta inexistente em produto headless
Robô de mesa sem cabo serial = crash perdido.
**Melhoria:** coredump para flash (adicionar partição `coredump` ~64 KB no `partitions.csv` — há 7,8 MB de storage para ceder espaço) e, no boot seguinte, enviar para o server via bridge + registrar em `/sdcard/logs`.
**Evolução:** "última fotografia" — snapshot do diagnostics gravado junto (estado, emoção, fila do bus, heap) para correlacionar com o dump.

### B6. OTA funcional mas sem rede de segurança
`esp_ota_begin/write/end/set_boot_partition` corretos, mas não há rollback automático nem validação de versão/projeto.
**Melhoria:** habilitar `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`; após boot da imagem nova, só chamar `esp_ota_mark_app_valid_cancel_rollback()` depois de um health check (boot completo + N segundos sem panic). Validar `esp_app_desc_t.project_name` antes de gravar.
**Evolução:** OTA A/B real (ota_1) se o espaço da storage permitir após re-layout do B5.

### B7. wake_service — acoplamento frágil com ESP-SR
Declarações `extern` manuais das funções de VAD do ESP-SR para evitar conflito de headers: se a assinatura mudar numa atualização do componente, é UB silencioso, não erro de compilação.
**Melhoria:** isolar num wrapper único (`nb_esp_sr_compat.c`) que inclua o header real num translation unit separado, com `_Static_assert` de versão do componente.
**Evolução:** thresholds (0.55f, ganhos, RMS mínimo) para NVS/config — calibração por ambiente sem reflash, seguindo o precedente do touch 2.2A.

### B8. Áudio v1/v2 — migração a meio caminho
audio_service (1.934 linhas) inicializa parte dos v2 e mantém pipeline própria; web_service inclui as duas gerações.
**Melhoria:** terminar a migração: v2 canônico, audio_service vira facade fina de compatibilidade, deletar caminhos mortos. Documentar em cada header quem é dono de I2S0/I2S1.
**Evolução:** quando estabilizar, o slot "AEC e duplex avançado" do backlog ganha base limpa.

### B9. conductor — dois refinamentos
(1) `conductor_pause` é bool global sem dono: web_service pausa para calibração; se outro chamador pausar/despausar, conflito silencioso. Trocar por refcount ou token de dono.
(2) Partituras hardcoded em ROM: ótimo para robustez, ruim para iteração de design.
**Evolução:** loader opcional de partituras de `/sdcard/assets/scores/*.json` em dev (fallback para ROM), permitindo ajustar animações sem reflash — alinha com a filosofia de calibração do touch.

### B10. Logger — flush para SD ainda é placeholder
O comentário descreve o design certo (ring buffer PSRAM + flush via persistence a cada 60 s), mas não está implementado. Agenda (14.1) e diagnóstico de campo dependem de log persistente.
**Melhoria:** implementar a Etapa 0.3 como descrita no próprio comentário. Elevar prioridade: é pré-requisito de observabilidade de tudo o mais.

### B11. Build de validação em -Og
`CONFIG_COMPILER_OPTIMIZATION_DEBUG=y` no sdkconfig ativo. Medições de FPS/latência/CPU ficam irreais e há folga desperdiçada.
**Melhoria:** dois perfis: `sdkconfig.defaults` com -O2 (validação/campo) e `sdkconfig.debug.defaults` (bancada). Toda medição de performance citada em docs deve dizer em qual perfil foi feita.

### B12. Documentação central defasada do código
CLAUDE.md: tabela de componentes sem web_service, bridge, wifi, vision*, v2, boredom, voice_controller, ui_overlay, diagnostics, i2c_hal, camera_hal; "WiFi desabilitado nos Blocos 0–8" com WiFi ativo; câmera "Adiado" com stack de visão em produção via server. A etapa D.1 já existe — **incluir CLAUDE.md explicitamente no escopo dela**, porque é o arquivo que instrui a IA e hoje instrui errado.

---

## C. O que está ERRADO — corrigir

### C1. `servo_test` no app_main é uma mina terrestre armada
`main.c` chama `nb_servo_test_ping()` antes de tudo, com `NB_SERVO_TEST_ENABLE_MOTION=1`. Hoje não move nada porque os servos estão fisicamente desconectados (ping falha → motion pulado). **Mas no dia em que alguém plugar os servos para começar a fase 5.x, o robô se move no primeiro boot**, antes de qualquer safety, com firmware que nem inicializa motion_safety.
**Correção:** remover a chamada de main.c agora (custo: 2 linhas). Mover servo_test para componente de diagnóstico atrás de Kconfig, executável só com motion_safety armado.

### C2. Endpoint HTTP de calibração — mesma mina, acionável pela LAN
Bypass deliberado de safety chamando servo_hal direto, 8× fire-and-forget, sem autenticação. Inofensivo com servos desconectados; perigoso no instante da reconexão, e errado como precedente arquitetural (Layer 2 → HAL).
**Correção:** remover ou guardar atrás de um modo de calibração formal (ver D4) que exija safety armado + confirmação física + token.

### C3. motion_safety tem races que precisam morrer ANTES da reintegração
Detalhado no relatório anterior (itens 4–7): transições de `s_state` de 3 contextos sem atomicidade (brownout durante arm() pode terminar ARMED), `BROWNOUT_WARN` nunca publicado (handler dead code), park escreve posição e desliga torque no instante seguinte (parking nunca executa), e-stop sem orçamento de latência garantido contra contenção do bus mutex.
**Correção:** tratar como **pré-condição bloqueante da etapa 5.x** (ver roadmap E). Nenhum servo conectado antes desse pacote fechar.

### C4. EMI no bus do servo mascarada com "mandar 8×"
20–30% de perda de pacotes com WiFi ativo (GPIO 19/20 = USB D±), documentada no código e compensada estatisticamente. Para comandos de posição, réplica atrasada = movimento fantasma.
**Correção:** problema de hardware da fase 5.x: medir taxa de erro como métrica, write+read-back como protocolo, e decidir mitigação física (pinos alternativos respeitando DVP, resistor série, cabo blindado) antes de qualquer movimento real.

### C5. web_service inverte a arquitetura
5.033 linhas em infra/ incluindo headers das Layers 1–7, chamadas para cima (`conductor_pause`), HAL direto, 11 subscriptions. É hoje o maior risco de manutenibilidade do firmware — qualquer mudança em qualquer camada pode quebrá-lo, e ele pode quebrar qualquer camada.
**Correção:** reclassificar como adapter de borda em `components/interface/web/`, quebrar por domínio (api_system, api_motion, api_audio, api_vision, api_ota), cada módulo falando só com facades de serviço. Fazer incrementalmente: extrair OTA primeiro (mais isolado), depois vision, depois o resto.

### C6. Endpoints HTTP mutadores sem autenticação + credenciais WiFi no binário
Reboot, OTA, config e (futura) calibração abertos a qualquer dispositivo na LAN. `wifi_creds.h` compila SSID/senha em texto plano na imagem flashada (e em qualquer .bin distribuído via OTA).
**Correção:** token por dispositivo gerado no primeiro boot (NVS, exibido no display), exigido em todo POST. Provisioning de WiFi via NVS/SoftAP eliminando o header (ver D7).

### C7. Pequenos erros objetivos
`power_monitor_set_mode` sem proteção de concorrência e sem publicar evento (viola a própria regra do CLAUDE.md); `motion_safety_emergency_stop` não publica `NB_EVT_MOTION_FAULT`; comentário do watchdog diz 5 s e o sdkconfig 10 s; comentários contraditórios sobre eco/flush em `send_packet` do servo_hal. Corrigir em passada única de higiene.

---

## D. O que NÃO EXISTE e deveria ser criado

### D1. `nb_task_config.h` — fonte única de tasks
Dezenas de `xTaskCreate*` com prioridade/core/stack definidos localmente. Criar header único com todas as constantes + tabela em ARCHITECTURE.md + script de build que confere se nenhum componente cria task fora da tabela. Resolve também a questão da prioridade do dispatcher (8) estar abaixo das tasks que dependem dele para receber eventos de safety.

### D2. Supervisor de saúde (Layer 3)
Hoje há peças soltas: diagnostics calcula health_score, watchdog alimenta TWDT, eventos de drop existem. Falta o componente que **junta**: consome check-ins de tasks (B2), heap/PSRAM livre, drops do bus, taxa de erro do servo bus, e publica `NB_EVT_HEALTH_DEGRADED`/`RECOVERED`. O status rail (16.2) e o LED viram a interface visível disso — sinergia direta com o ciclo atual.

### D3. Monitor de memória com a regra dos 300 KB
A regra "manter >300 KB de PSRAM livres" existe só como comentário. Criar verificação periódica no supervisor (D2): abaixo do limiar → evento + recusa de features opcionais (preview de câmera é o candidato óbvio a degradar primeiro).

### D4. Modo MAINTENANCE na state_machine
Estado formal que agrupa tudo que hoje é gambiarra: calibração de servo, OTA, testes de diagnóstico, registro facial. Entrada por ação física (toque longo) + token; saída por timeout; baseline visual próprio (já permitido pela regra de estados com base própria). Tudo que é "bypass" vira sub-modo auditável com regras claras.

### D5. Testes de firmware em host + CI
Existe pytest para server/bridge, zero testes para o firmware. Os componentes mais valiosos de testar são lógica pura sem hardware: state_machine, behavior_engine (tabela de regras!), emotion_model, event_bus, conductor (sequenciamento), protocolo SCS (checksum/parse). Criar build host (Linux/CMake com stubs de FreeRTOS/esp_timer) rodando em CI a cada commit. O parse de pacote do servo_hal testado em host teria pego a questão do eco/flush.

### D6. Gate de reintegração de motion (checklist executável)
Antes da etapa 5.x: documento + script que verifica (a) C3 corrigido com teste de concorrência em host, (b) taxa de erro do bus UART < limiar com WiFi ativo, (c) orçamento de e-stop medido < 150 ms no pior caso, (d) servo_test removido do main, (e) endpoint de calibração atrás do modo MAINTENANCE. Sem os 5 verdes, servo não conecta. Transforma a regra "motion_safety verde" de princípio em processo.

### D7. Provisioning WiFi + identidade de dispositivo
SoftAP de primeiro boot (ou config via SD: `/sdcard/config/wifi.json` lido uma vez e apagado) gravando em NVS; token de API gerado e mostrado no display. Elimina wifi_creds.h e destrava distribuir builds.

### D8. Caixa-preta de voo
Combinação de B5 + B10: ring buffer de eventos do bus (últimos ~200) + logs + snapshot de diag, persistidos em crash e no boot seguinte ao server. Para um produto que vive na mesa de outra pessoa, é a diferença entre "travou, sei lá" e diagnóstico remoto.

### D9. Gesto universal de "para tudo"
Toque longo (ou padrão de toque duplo) = interrupção comportamental total: conductor interrompe, áudio para, expressão volta a NEUTRAL, (futuro) motion congela. Custa pouco (os mecanismos de interrupção já existem) e dá ao usuário a sensação de controle que produto de robótica social precisa.

### D10. Orçamento de energia instrumentado
ENERGY.md descreve o orçamento teórico; nada mede. Quando o hardware de bateria voltar, será tarde para descobrir surpresas. Barato agora: ADC no 5 V (mesmo ponto que servirá ao brownout runtime de C3) + logging do supervisor. Um sensor, dois requisitos.

---

## E. Roadmap revisado

Mantém a fila atual (o foco em agenda/status rail/docs está certo) e injeta o trabalho desta análise em pacotes pequenos. Nomenclatura: R0–R4 para não colidir com as etapas existentes.

### R0 — Desarmar minas (fazer JÁ, ~1 dia, paralelo ao P0 atual)
Remover `nb_servo_test_ping()` do main.c e mover servo_test para Kconfig (C1). Desabilitar/guardar endpoint de calibração (C2). Token de API mínimo nos POSTs mutadores (C6 parcial). Passada de higiene C7.
*Critério de saída: nenhum caminho de código pode escrever em servo; nenhum endpoint mutador sem token.*

### P0 atual — continua igual
14.1 Agenda local · 16.2 Status rail · D.1 Limpeza documental (**adicionar CLAUDE.md ao escopo**, B12).

### R1 — Fundação e observabilidade (próximo ciclo, intercalável com P0)
B1 config cache RAM · B2 watchdog check-in · B3 event bus (slots de safety + asserts + política sync) · B10 logger→SD · B5 coredump em flash · B11 perfis de build · D1 nb_task_config.h · D2/D3 supervisor de saúde + monitor de memória (alimenta o status rail 16.2 — fazer junto rende dobrado).
*Critério de saída: crash gera artefato recuperável; task travada gera panic; status rail mostra saúde real.*

### R2 — Pré-condições de motion (antes da etapa 5.x existente; só quando decidirem reconectar servos)
C3 completo (transições atômicas, brownout runtime via ADC/Present Voltage, e-stop budget, choke point com write-guard na HAL) · C4 EMI medida e mitigada · D6 gate executável · D4 modo MAINTENANCE absorvendo a calibração.
*Critério de saída: os 5 itens do gate D6 verdes.*

### R3 — Dívida arquitetural (fundo de fila, incremental)
C5 decomposição do web_service (começar por OTA) · B8 unificação áudio v2 · B6 OTA rollback · B7 wrapper ESP-SR · D7 provisioning WiFi.

### R4 — Qualidade contínua (permanente)
D5 testes host + CI · B3-evolução telemetria de bus · B9 partituras em SD (dev) · D8 caixa-preta · D9 gesto para-tudo · D10 instrumentação de energia.

### Regra transversal proposta
Toda fase skipada no boot (`phase_skip`) deve aparecer como pendência visível no diagnostics/status rail — "desativado temporariamente" nunca mais como estado invisível.
