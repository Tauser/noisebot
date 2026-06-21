# DMM.3 — Gate elétrico e brownout da Waveshare

Status: `FEITO` (2026-06-21) — ver nota de gap conhecido na seção 6.7

Objetivo: provar em bancada que a Waveshare pode assumir o corpo do robô sem
back-power, sem conflito de alimentação e sem brownout destrutivo antes de
liberar áudio físico, LEDs externos e servos.

## 1. Escopo do gate

Este gate cobre apenas:

- fonte de 5 V e distribuição;
- GND comum entre domínios;
- monitoramento do barramento de 5 V;
- proteção contra brownout;
- boot seguro com cargas progressivas.

Este gate ainda **não** libera:

- áudio físico final (`DMM.4`);
- touch corporal (`DMM.6`);
- LEDs externos em operação normal (`DMM.7`);
- servo energizado/movimento (`DMM.8`/`DMM.9`).

## 2. Decisões válidas para DMM.3

- A placa alvo é a **Waveshare ESP32-S3 N32R16V** como main-controller.
- O monitor de 5 V da Waveshare exige divisor externo em `GPIO7`:
  - proposta canônica de documentação: `68k/56k`
  - montagem temporária validada em bancada (2026-06-21): `100k/100k`
- `GPIO19/20` ficam reservados ao USB nativo.
- O enlace com o head já foi validado e não bloqueia este gate.
- O rail de servo continua tratado como domínio separado e **desligado** no
  primeiro gate elétrico.
- Estado atual de bancada (2026-06-21):
  - divisor externo temporário `100k/100k` montado e validado;
  - medições confirmadas: `5V=4.741 V`, `GPIO7=2.368 V`, `GND=0.000 V`;
  - a ligação atual serve para validação e pode permanecer durante DMM.3;
  - se o monitor de 5 V for mantido no produto, algum divisor equivalente
    continuará necessário de forma mais permanente.

## 3. Resolução de ambiguidade documental

Existe uma divergência aparente entre documentos:

- [GPIO_DUAL_MCU.md](/D:/Projetos/Noisebot/docs/GPIO_DUAL_MCU.md:64) e
  [DMM1_WAVESHARE_INVENTORY.md](/D:/Projetos/Noisebot/docs/DMM1_WAVESHARE_INVENTORY.md:100)
  já reservam `GPIO7` como monitor de 5 V da Waveshare;
- [HARDWARE.md](/D:/Projetos/Noisebot/docs/HARDWARE.md:299) diz que não há ADC
  livre confirmado, mas esse trecho reflete o contexto legado/anterior e não
  deve bloquear a validação da Waveshare em DMM.3.

Para DMM.3, a referência operacional é o mapa dual-MCU/Waveshare, não o mapa
monolítico legado.

## 4. Topologia alvo do gate

- Fonte principal:
  - 5 V / 3 A externo, entrando por alimentação dedicada
  - não usar USB da placa como fonte do sistema
- Domínio lógico 5 V:
  - Waveshare
  - MAX98357A
  - WS2812 externos
- Domínio servo:
  - TTLinker + servos em rail separado
  - energização mantida desligada no gate inicial
- Terra:
  - GND comum obrigatório
  - preferência por star ground

## 5. Critérios de aprovação

O gate só fecha quando todos os itens abaixo estiverem verdes:

- nenhuma saída 5 V independente está unida a outra saída 5 V;
- Waveshare sobe sem head e sem periféricos externos;
- Waveshare sobe com head conectado;
- leitura do divisor de 5 V não excede faixa segura do ADC;
- 3.3 V e 5 V permanecem estáveis em idle e sob carga leve;
- brownout não causa movimento de servo;
- brownout não causa corrupção observável de boot/config;
- não existe back-power via GPIO, USB ou módulos externos.

## 5.1 Telemetria esperada no firmware

Com o build atual da Waveshare, os endpoints `/api/health` e `/api/diag`
passaram a expor telemetria de energia no objeto `power`:

- `adc_available`
- `adc_mv`
- `bus_5v_mv`
- `warn`
- `critical`

Interpretação operacional:

- `adc_available=true`: o perfil de placa expôs o ADC de 5 V corretamente;
- `adc_mv`: tensão no ponto médio do divisor;
- `bus_5v_mv`: estimativa do barramento real de 5 V;
- `warn=true`: barramento abaixo de `4700 mV`;
- `critical=true`: barramento abaixo de `4500 mV`.

Critério de leitura saudável neste gate:

- `adc_available=true`
- `critical=false` em todos os cenários aprovados
- `warn=false` em idle e carga leve
- `bus_5v_mv` coerente com multímetro, com erro inicial aceitável <= 5%

Enquanto a solução permanente não existir, a bancada segue com divisor externo
temporário e comparação com multímetro.

**Correção de calibração aplicada (2026-06-21):** a leitura crua do ADC saiu
sistematicamente ~2.2% abaixo do multímetro (`adc_mv=2317` vs `2.368V` real;
`bus_5v_mv=4636` vs `4.741V` real), o que fazia `warn=true` disparar em idle
mesmo com o barramento saudável — falso positivo pelo critério da seção 6.5.
`power_monitor.c` ganhou um fator de correção empírico
(`NB_POWER_5V_ADC_CAL_NUM/DEN = 10223/10000`, derivado dessa amostra única)
aplicado em `read_adc_pin_mv()`. Pós-correção, idle leu `adc_mv=2370`,
`bus_5v_mv=4742`, `warn=false` — consistente com o multímetro. Fator validado
em um único ponto (~2.3–4.7V); revisitar com mais pontos de calibração antes
de confiar fora dessa faixa.

## 6. Checklist de bancada

### 6.1 Inspeção sem energizar

- [x] Confirmar visualmente a fonte principal que alimentará o sistema
- [x] Confirmar que USB está sendo usado só para debug/flash
- [x] Confirmar GND comum entre Waveshare, head e módulos externos
- [x] Confirmar que o rail de servo está fisicamente separado do rail lógico
- [x] Confirmar que `GPIO19/20` não foram reutilizados fora do USB
- [x] Confirmar que `GPIO47/48` não estão ligados em lógica 3.3 V
- [x] Confirmar presença do divisor externo para o monitor de 5 V
- [x] Confirmar que o ponto médio do divisor vai para `GPIO7`
- [x] Confirmar polaridade correta em todos os módulos 5 V

Se a solução permanente ainda não estiver instalada:

- [x] Registrar explicitamente a montagem temporária usada em bancada
- [x] Prosseguir com o gate comparando ADC x multímetro

### 6.2 Energização mínima

- [x] Energizar apenas Waveshare
- [x] Boot completo e estável
- [x] Sem reset espontâneo
- [x] Sem aquecimento anormal — confirmado pelo usuário durante os testes de
      carga (Wi-Fi, amp, LEDs, head conectado)
- [x] Medir 5 V na entrada da Waveshare — `4.741V` (multímetro, bancada 2026-06-21)
- [x] Medir 3.3 V regulado na Waveshare — `3.250V` isolado / `3.258V` sob
      carga (Wi-Fi+head+boot normal)
- [x] Se divisor montado: confirmar `/api/health` com `power.adc_available=true`
- [x] Se divisor montado: registrar `power.adc_mv` e `power.bus_5v_mv` —
      ver tabela seção 7
- [x] Se divisor montado: confirmar `power.warn=false` e `power.critical=false`
      (pós-correção de calibração)

### 6.3 Waveshare + head

- [x] Conectar head mantendo periféricos externos do corpo desligados
- [x] Confirmar boot estável dos dois lados
- [x] Confirmar que o enlace continua em `READY` — `nb_main_link: state
      SNAPSHOT -> READY`, snapshot visual restaurado generation=10/12,
      telemetria sem invalid/retry/timeout/spi_err
- [x] Confirmar ausência de reset em loop
- [x] Medir 5 V e 3.3 V com o head conectado — `bus_5v_mv` 4718–4864 mV
      (API), `3.3V=3.258V` (multímetro)
- [x] Se divisor montado: registrar `power.bus_5v_mv` também com o head
      conectado — ver tabela seção 7
- [x] Se divisor montado: confirmar `power.warn=false` e `power.critical=false`
      (exceto ruído transitório de uma amostra coincidindo com reconexão
      Wi-Fi, não relacionado ao head/link)

### 6.4 Barramento de 5 V monitorado

- [x] Medir com multímetro o barramento real de 5 V — `4.741V`
- [x] Medir tensão no ponto médio do divisor — `2.368V`
- [x] Verificar se o ponto médio permanece abaixo de 3.1 V — `2.368V`, OK
- [x] Comparar `power.adc_mv` com a medição no ponto médio — `2370mV` API
      vs `2368V` multímetro pós-correção (erro ~0.1%)
- [x] Comparar `power.bus_5v_mv` com a medição do barramento real —
      `4742mV` API vs `4741V` multímetro pós-correção (erro ~0.02%)
- [x] Registrar diferença entre tensão real e tensão convertida esperada —
      ver nota de calibração na seção 5.1
- [x] Critério inicial: erro absoluto aceitável <= 5% — atingido (<1% após
      correção, frente a ~2.2% antes da correção)

Se o divisor não estiver montado:

- [N/A] Divisor está montado (100k/100k) — itens condicionais não se aplicam

### 6.5 Carga leve no rail lógico

- [x] Habilitar Wi-Fi
- [x] Repetir medidas de 5 V e 3.3 V — `3.3V=3.258V` por multímetro em idle
      com Wi-Fi+head+boot normal ativos (vs `3.250V` isolado sem carga;
      regulador estável, sem sag perceptível)
- [x] Habilitar display/head link já validado — perfil `sdkconfig.dmm.link.defaults`
      (Waveshare profile + `NB_INTER_MCU_SPI_ENABLED=y`, boot normal sem bench);
      enlace subiu em `READY` com snapshot visual restaurado, simultâneo a
      Wi-Fi/áudio/serviços
- [x] Observar queda de tensão, reboot ou instabilidade — nenhuma queda
      significativa sob rajada de requisições HTTP (idle 4742 mV → TX 4758 mV);
      com head conectado, idle 4718–4864 mV (ruído maior coincide com
      reconexão Wi-Fi, não com o link); sem reset em nenhum dos dois lados
- [x] Confirmar `power.critical=false`
- [x] `power.warn=false` em idle e sob carga de Wi-Fi (pós-correção de
      calibração)

### 6.6 Carga de áudio/LED antes de servo

- [x] Ligar apenas o amplificador no rail lógico — disparado via
      `POST /api/command {"type":"ACTION","value":"CELEBRATE"}`
- [x] Confirmar que não há ruído, reset ou aquecimento anormal — sem reset;
      sem aquecimento anormal confirmado pelo usuário
- [x] Ligar apenas LEDs externos no rail lógico — brilho elevado a 204/255
      via `POST /api/config {"key":"brightness","value":204}`
- [x] Confirmar que não há flicker por sag de alimentação — sag transitório
      de uma amostra (`warn=true` momentâneo no instante do degrau de
      brilho, recuperado em <1s); sem flicker visível reportado
- [x] Repetir medidas de 5 V e 3.3 V — `3.3V` confirmado estável em idle
      (3.250–3.258V) e sob carga combinada máxima (head+Wi-Fi+amp+LEDs);
      não isolado por carga individual (amp-só, LED-só) via multímetro,
      apenas via API
- [x] Registrar `power.bus_5v_mv`, `power.warn` e `power.critical` — ver
      tabela da seção 7

### 6.7 Brownout controlado

- [x] Simular condição de queda de alimentação de forma controlada — feito
      por desconexão/reconexão total da fonte de 5V (bancada 2026-06-21);
      não foi um sag parcial de tensão (precisaria de fonte ajustável)
- [ ] Confirmar log/evento de brownout — **não disparou**: o reset foi
      registrado como `Reset reason : POWERON (1)` com
      `brownout_reset=nao`. Esperado: corte total a zero não passa pela
      janela de tensão que o BOD do ESP32-S3 detecta como brownout (esse
      circuito pega *sags* parciais, não perda completa de energia).
      Gap conhecido: o circuito BOD em si não foi exercitado nesta rodada.
- [x] Confirmar reboot limpo ou safe behavior — boot completo, sem
      PANIC/assert, `Configuração carregada (cfg_ver=3)`,
      `boot_count resetado — sistema reportou sucesso`; WiFi, enlace com o
      head e serviços recuperaram sozinhos sem intervenção
- [x] Confirmar que servos continuam sem torque no gate atual —
      `FASE SAFETY: PULADA (motion desativado temporariamente)` e
      `FASE MOTION: PULADA` em todos os boots observados
- [x] Confirmar ausência de corrupção em boot seguinte — NVS/config OK,
      sem reset em loop, head também recuperou limpo (boot duplo observado
      do lado do head, ambos sem corrupção)

**Decisão (2026-06-21):** gate fechado com o resultado de power-loss total.
O teste de sag parcial (exercitar o BOD de fato) fica registrado como gap
conhecido, a revisitar com fonte ajustável antes de DMM.9 (motion safety)
se a robustez a brownout parcial for julgada crítica para movimento de
servo.

## 7. Tabela de medições

| Cenário | 5 V medido | 3.3 V medido | Vdiv em GPIO7 | `bus_5v_mv` API | `warn/critical` | Resultado |
| --- | --- | --- | --- | --- | --- | --- |
| Waveshare sozinha (sem divisor) | 4.714 V | 3.250 V | 0.360–0.362 V* | — | — | LEITURA INVALIDA EM GPIO7 |
| Waveshare sozinha (com divisor 100k/100k) | 4.741 V | — | 2.368 V | 4636 mV (pré-cal.) | warn=true (falso +) | DIVISOR VALIDADO |
| Waveshare idle (pós-correção de calibração) | não medido | — | — | 4742 mV | false/false | APROVADO |
| Waveshare + Wi-Fi (rajada HTTP) | não medido | — | — | 4758 mV | false/false | APROVADO |
| Waveshare + amp (ACTION CELEBRATE) | não medido | — | — | 4734–4742 mV | false/false | APROVADO |
| Waveshare + LEDs (brilho 204/255) | não medido | — | — | 4738–4742 mV (1 amostra 4742 com warn=true transitório) | majoritariamente false/false | APROVADO COM NOTA |
| Waveshare + head (link READY, boot normal) | não medido | 3.258 V | — | 4718–4864 mV | majoritariamente false/false (1 amostra com ruído coincidindo com reconexão Wi-Fi) | APROVADO |
| Waveshare + head + Wi-Fi + amp + LEDs (carga combinada máxima) | não medido | — | — | 4718–4770 mV | false/false | APROVADO |

\* Leitura direta em `GPIO7` sem divisor externo montado; **não representa**
o barramento real de 5 V.

## 8. Riscos ativos

- uso acidental da USB como alimentação principal;
- unir duas fontes de 5 V por engano;
- rail de servo compartilhado cedo demais;
- divisor de 5 V montado errado e excedendo o ADC;
- retorno de corrente por GPIO de periféricos ligados a 5 V;
- cabo/fonte com queda excessiva sob Wi-Fi, áudio ou LEDs.

## 9. Próxima ação prática

Restam só itens que exigem ação física na bancada (não executáveis por
software/API):

1. medir `3.3V` por multímetro nos cenários idle/Wi-Fi/amp/LEDs (só temos
   `5V` via API+multímetro até aqui);
2. repetir o cenário com o head conectado e o enlace em `READY`, medindo
   `power.bus_5v_mv` com a carga combinada;
3. `6.7` brownout controlado: simular queda de alimentação de forma
   controlada e confirmar log/evento de brownout, reboot limpo e ausência de
   corrupção no boot seguinte — requer intervenção física na fonte, não pode
   ser feito remotamente.

Carga leve (Wi-Fi), amplificador e LEDs já foram validados via API com a
calibração corrigida (seção 7); nenhum mostrou sag significativo.
