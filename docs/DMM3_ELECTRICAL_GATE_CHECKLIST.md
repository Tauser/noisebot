# DMM.3 — Gate elétrico e brownout da Waveshare

Status: `EM ANDAMENTO`

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

## 6. Checklist de bancada

### 6.1 Inspeção sem energizar

- [ ] Confirmar visualmente a fonte principal que alimentará o sistema
- [ ] Confirmar que USB está sendo usado só para debug/flash
- [ ] Confirmar GND comum entre Waveshare, head e módulos externos
- [ ] Confirmar que o rail de servo está fisicamente separado do rail lógico
- [ ] Confirmar que `GPIO19/20` não foram reutilizados fora do USB
- [ ] Confirmar que `GPIO47/48` não estão ligados em lógica 3.3 V
- [x] Confirmar presença do divisor externo para o monitor de 5 V
- [x] Confirmar que o ponto médio do divisor vai para `GPIO7`
- [ ] Confirmar polaridade correta em todos os módulos 5 V

Se a solução permanente ainda não estiver instalada:

- [x] Registrar explicitamente a montagem temporária usada em bancada
- [x] Prosseguir com o gate comparando ADC x multímetro

### 6.2 Energização mínima

- [ ] Energizar apenas Waveshare
- [ ] Boot completo e estável
- [ ] Sem reset espontâneo
- [ ] Sem aquecimento anormal
- [ ] Medir 5 V na entrada da Waveshare
- [ ] Medir 3.3 V regulado na Waveshare
- [ ] Se divisor montado: confirmar `/api/health` com `power.adc_available=true`
- [ ] Se divisor montado: registrar `power.adc_mv` e `power.bus_5v_mv`
- [ ] Se divisor montado: confirmar `power.warn=false` e `power.critical=false`

### 6.3 Waveshare + head

- [ ] Conectar head mantendo periféricos externos do corpo desligados
- [ ] Confirmar boot estável dos dois lados
- [ ] Confirmar que o enlace continua em `READY`
- [ ] Confirmar ausência de reset em loop
- [ ] Medir 5 V e 3.3 V com o head conectado
- [ ] Se divisor montado: registrar `power.bus_5v_mv` também com o head conectado
- [ ] Se divisor montado: confirmar `power.warn=false` e `power.critical=false`

### 6.4 Barramento de 5 V monitorado

- [ ] Medir com multímetro o barramento real de 5 V
- [ ] Medir tensão no ponto médio do divisor
- [ ] Verificar se o ponto médio permanece abaixo de 3.1 V
- [ ] Comparar `power.adc_mv` com a medição no ponto médio
- [ ] Comparar `power.bus_5v_mv` com a medição do barramento real
- [ ] Registrar diferença entre tensão real e tensão convertida esperada
- [ ] Critério inicial: erro absoluto aceitável <= 5%

Se o divisor não estiver montado:

- [ ] Marcar esta seção como "PENDENTE — resistores externos ausentes"
- [ ] Não interpretar leitura crua de `GPIO7` como barramento de 5 V

### 6.5 Carga leve no rail lógico

- [ ] Habilitar Wi-Fi
- [ ] Repetir medidas de 5 V e 3.3 V
- [ ] Habilitar display/head link já validado
- [ ] Observar queda de tensão, reboot ou instabilidade
- [ ] Confirmar `power.critical=false`
- [ ] Se `power.warn=true`, registrar cenário como reprovado para avanço

### 6.6 Carga de áudio/LED antes de servo

- [ ] Ligar apenas o amplificador no rail lógico
- [ ] Confirmar que não há ruído, reset ou aquecimento anormal
- [ ] Ligar apenas LEDs externos no rail lógico
- [ ] Confirmar que não há flicker por sag de alimentação
- [ ] Repetir medidas de 5 V e 3.3 V
- [ ] Registrar `power.bus_5v_mv`, `power.warn` e `power.critical`

### 6.7 Brownout controlado

- [ ] Simular condição de queda de alimentação de forma controlada
- [ ] Confirmar log/evento de brownout
- [ ] Confirmar reboot limpo ou safe behavior
- [ ] Confirmar que servos continuam sem torque no gate atual
- [ ] Confirmar ausência de corrupção em boot seguinte

## 7. Tabela de medições

| Cenário | 5 V medido | 3.3 V medido | Vdiv em GPIO7 | `bus_5v_mv` API | `warn/critical` | Resultado |
| --- | --- | --- | --- | --- | --- | --- |
| Waveshare sozinha (sem divisor) | 4.714 V | 3.250 V | 0.360–0.362 V* | — | — | LEITURA INVALIDA EM GPIO7 |
| Waveshare sozinha (com divisor 100k/100k) | 4.741 V | — | 2.368 V | pendente | pendente | DIVISOR VALIDADO |
| Waveshare + head | — | — | — | — | — | PENDENTE |
| Waveshare + Wi-Fi | — | — | — | — | — | PENDENTE |
| Waveshare + amp | — | — | — | — | — | PENDENTE |
| Waveshare + LEDs | — | — | — | — | — | PENDENTE |

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

Próximo corte recomendado de bancada:

1. Waveshare sozinha;
2. Waveshare + head;
3. medir `5V` e `3.3V` por multímetro;
4. usar `GPIO7`/`power.*` com o divisor temporário `100k/100k` já validado;
5. só depois adicionar amp e LEDs;
6. manter servo rail desligado neste primeiro gate.
