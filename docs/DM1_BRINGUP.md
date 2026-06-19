# DM1 — Bring-up físico do enlace dual-MCU

## Objetivo

Validar somente o enlace entre a Waveshare N32R16 main e a Freenove N16R8
head: alimentação, SPI, IRQ, reset, handshake, heartbeat, recuperação e
telemetria. Esta etapa não migra display, câmera, SD ou comportamento.

O baseline normal permanece com `CONFIG_NB_INTER_MCU_SPI_ENABLED=n`. Os perfis
`sdkconfig.dm1.defaults` são exclusivos de bancada.

## Condições de bloqueio

Não conectar nem energizar o enlace se qualquer item abaixo estiver pendente:

- variante e silk da Waveshare N32R16 em mãos não conferidas;
- GPIO47/48 da Waveshare usados como lógica 3,3 V;
- áudio ainda dirigindo GPIO1/14/41/42 da Freenove;
- touch corporal ainda dirigindo/ocupando GPIO2 da Freenove;
- ausência de GND comum entre as placas;
- curto ou resistência anormal entre 3,3 V e GND;
- qualquer GPIO do enlace acima de 3,3 V;
- servos, amplificador, alto-falante ou LEDs de potência alimentados durante o
  primeiro bring-up.

GPIO19/20 da Freenove pertencem ao legado de servo, mas não fazem parte do
enlace. Servo, áudio, LEDs externos e touch corporal devem permanecer
desconectados durante DM1.

## Instrumentação mínima

- multímetro;
- fonte USB/fonte de bancada individual para cada placa;
- dois cabos USB/UART independentes para logs;
- analisador lógico ou osciloscópio recomendado;
- cabos SPI curtos, idealmente até 15 cm;
- pelo menos dois condutores GND entre as placas, próximos a SCLK e dados.

Não unir as linhas de 5 V das duas fontes no primeiro teste. Unir apenas GND e
os sinais de lógica. Nenhuma placa pode alimentar a outra pelos GPIOs.

## Pinagem

| Sinal | Main Waveshare | Head Freenove | Direção |
| --- | --- | --- | --- |
| `LINK_SCLK` | GPIO12 | GPIO41 | main → head |
| `LINK_MOSI` | GPIO11 | GPIO42 | main → head |
| `LINK_MISO` | GPIO13 | GPIO14 | head → main |
| `LINK_CS` | GPIO10 | GPIO1 | main → head |
| `HEAD_IRQ` | GPIO14 | GPIO2 | head → main |
| `HEAD_RESET` | GPIO8 | EN | main → head |
| GND | GND | GND | comum |

Não conectar `HEAD_RESET` no primeiro teste lógico. Validá-lo separadamente
depois que handshake e heartbeat estiverem estáveis.

## Gate E0 — placas isoladas

Com as placas desligadas e sem fios entre elas:

1. Confirmar continuidade de cada cabo e ausência de curto entre cabos.
2. Medir resistência entre 3,3 V e GND de cada placa; investigar leitura
   próxima de zero.
3. Remover microSD e desconectar câmera/display apenas se isso for necessário
   para garantir que o firmware de teste não ative periféricos legados.
4. Desconectar servo, TTLinker, amplificador, microfone, LEDs externos e touch.
5. Gravar primeiro os firmwares baseline com a flag desligada.
6. Energizar cada placa isoladamente e confirmar boot normal.

Aceite E0: nenhuma placa reinicia, aquece ou apresenta consumo anormal.

## Gate E1 — alta impedância do head

Ainda sem unir as placas, executar no head o firmware que será usado no DM1 e
verificar GPIO1, GPIO2, GPIO14, GPIO41 e GPIO42:

- antes de habilitar o perfil DM1, nenhum driver legado pode possuir os pinos;
- medir tensão estática e atividade com analisador lógico;
- não pode existir clock I2S, áudio, touch polling ou transição de LED;
- GPIO1/41/42 devem permanecer sem atividade até o SPI slave iniciar;
- GPIO14 e GPIO2 só podem mudar conforme o contrato do enlace.

Aceite E1: nenhum sinal legado detectado por pelo menos 60 segundos.

## Gate E2 — alimentação e GND

1. Desligar ambas as placas.
2. Manter fontes de 5 V/USB separadas.
3. Unir GND entre main e head.
4. Energizar uma placa por vez e medir diferença de potencial entre os GNDs.
5. A diferença deve ser menor que 50 mV.

Aceite E2: GND estável e nenhuma corrente perceptível por linhas de sinal.

## Build opt-in DM1

As tasks do VS Code criam configurações e builds independentes:

- `DM1: build main-controller`;
- `DM1: build head-controller`;
- `DM1: build ambos`.

Equivalente por terminal, dentro de cada projeto:

```text
idf.py -B build-dm1 -D SDKCONFIG=sdkconfig.dm1 -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.dm1.defaults" build
```

Confirmar no log/configuração gerada:

```text
CONFIG_NB_INTER_MCU_SPI_ENABLED=y
```

O build normal usa `build/` e `sdkconfig`; o DM1 usa `build-dm1/` e
`sdkconfig.dm1`. Não substituir o baseline.

No main, o perfil também habilita `CONFIG_NB_DM1_BENCH_PROFILE=y`. Esse modo
ignora integralmente o boot monolítico legado e sobe somente o serviço do
enlace. Isso impede display/SD/câmera/touch/áudio legados de disputarem GPIOs
ou o SPI2 durante a bancada.

## Gate E3 — link sem reset

Com tudo desligado:

1. Conectar GND, SCLK, MOSI, MISO, CS e IRQ.
2. Deixar `HEAD_RESET` desconectado.
3. Conferir continuidade pino a pino e ausência de inversão MOSI/MISO.
4. Energizar primeiro o head e abrir seu monitor.
5. Energizar o main e abrir seu monitor.

O bring-up começa em 10 MHz. Não promover a 20 MHz nesta etapa.

Aceite E3:

- ambos transitam `HANDSHAKE → SNAPSHOT → READY`;
- handshake conclui em até 1 segundo;
- nenhum `spi_err`;
- `invalid=0` depois da estabilização;
- heartbeat mantém ambos em `READY` por 30 minutos;
- `ack timeout=0` sem fault injection;
- nenhuma interferência em boot, Wi-Fi ou console do main.

## Gate E4 — integridade e latência

Coletar os resumos de telemetria emitidos a cada 5 segundos:

- `state`;
- `tx` e `rx`;
- `invalid`;
- `retry` e `timeout`;
- `spi_err`/`spi_to`;
- `hs`;
- `ack_last/avg/max`.

Critérios iniciais em 10 MHz:

| Métrica | Aceite |
| --- | --- |
| Estado | `READY` contínuo |
| Handshake | ≤ 1000 ms |
| Frames inválidos | 0 |
| ACK timeouts | 0 |
| SPI errors main | 0 |
| ACK RTT médio | ≤ 20 ms |
| ACK RTT máximo | ≤ 100 ms sem injeção |
| Link drops | 0 em 30 min |

`spi_to` no head é esperado quando o main não fornece clock; avaliar tendência,
não usar esse contador isoladamente como falha.

O slave mantém uma transação armada por até 100 ms; o main consulta a cada
20 ms. Essa margem evita corrida de fase entre timeouts iguais sem transformar
ausência do main em falha do head.

## Gate E5 — fault injection

Executar um caso por vez e registrar logs dos dois MCUs:

1. **Head ausente no boot:** main deve continuar operando; link não pode bloquear
   safety, áudio ou comportamento.
2. **Main ausente no boot:** head deve permanecer vivo e aguardar handshake.
3. **Remover SCLK por 2 segundos:** ambos degradam sem crash; ao reconectar,
   recuperam `READY`.
4. **Remover IRQ:** polling periódico deve recuperar tráfego e heartbeat.
5. **Reset manual do head:** main detecta novo `boot_id`, aborta pendências
   ambíguas e refaz snapshot.
6. **Reset manual do main:** head aceita novo `boot_id` e novo snapshot.
7. **Remover MISO:** ACKs expiram de forma limitada; nenhuma operação é
   considerada não executada apenas pelo timeout.
8. **Bit flip controlado:** usar analisador/injetor apropriado; CRC deve
   incrementar `invalid` sem entregar payload à aplicação.

Depois de cada caso, restaurar o cabeamento com as duas placas desligadas.

Aceite E5: recuperação sem reboot em loop, deadlock, crescimento ilimitado de
fila ou impacto em `motion_safety`.

### Folha de execução E5

Pré-condições:

- soak de 8 horas encerrado e telemetria final coletada;
- placas desligadas antes de remover ou recolocar qualquer fio;
- `HEAD_RESET` ainda desconectado;
- executar somente um defeito por energização;
- após cada caso, desligar, restaurar o cabeamento e repetir continuidade.

| Caso | Injeção | Evidência obrigatória | Resultado |
| --- | --- | --- | --- |
| E5.1 | head ausente no boot | main vivo, sem bloqueio de safety | aprovado: `HANDSHAKE`, TX=17 limitado, RX=0, erros=0 |
| E5.2 | main ausente no boot | head vivo aguardando handshake | aprovado: `HANDSHAKE`, TX/RX=0, `spi_err=0`, sem reboot |
| E5.3 | SCLK ausente por 2 s | degrada e recupera `READY` | parcial: boot sem SCLK estável; após power-cycle e restauração, `READY`, erros=0; main 3.295 ms, head 237 ms |
| E5.4 | IRQ ausente | polling mantém heartbeat/tráfego | aprovado: ambos `READY`, TX/RX crescente, erros=0 |
| E5.5 | reset manual do head | novo `boot_id`, snapshot refeito | aprovado após correção: ambos `READY`, erros=0; telemetria de duração corrigida em seguida |
| E5.6 | reset manual do main | novo `boot_id`, snapshot aceito | pendente |
| E5.7 | MISO ausente | timeout limitado, sem falsa conclusão | pendente |
| E5.8 | bit flip controlado | `invalid` cresce, payload não entregue | adiado sem injetor |

E5.8 não deve ser improvisado por curto, ruído manual ou contato intermitente.
Sem analisador/injetor apropriado, manter como gate de laboratório e usar o
teste host de CRC como cobertura provisória.

## Gate E6 — HEAD_RESET

Somente depois de E3–E5:

1. Desligar as placas.
2. Conectar main GPIO8 ao pino EN do head.
3. Confirmar que EN possui pull-up da placa e não recebe 5 V.
4. Energizar head e main.
5. Acionar um único reset comandado e observar pulso LOW de aproximadamente
   20 ms.
6. Confirmar espera de aproximadamente 100 ms antes da retomada.
7. Repetição antes de 10 segundos deve ser rejeitada pelo rate limit.

Aceite E6: apenas o head reinicia; main permanece operacional e refaz
handshake/snapshot.

### Registro E6

| Verificação | Esperado | Resultado |
| --- | --- | --- |
| EN do head em repouso | pull-up, sem 5 V | pendente |
| pulso main GPIO8→EN | LOW por aproximadamente 20 ms | pendente |
| retomada | espera de aproximadamente 100 ms | pendente |
| rate limit | segundo pedido antes de 10 s rejeitado | pendente |
| recuperação | novo `boot_id`, handshake e snapshot | pendente |
| isolamento | main não reinicia | pendente |

## Soak e promoção

Antes de considerar DM1 fisicamente verde:

- soak mínimo de 8 horas em 10 MHz;
- zero crash, watchdog ou brownout;
- zero frame inválido sem injeção;
- zero ACK timeout sem injeção;
- zero interferência em `motion_safety`;
- heap estável nos dois MCUs;
- logs e resultados anexados à decisão de bancada.

20 MHz só pode ser testado depois do soak de 10 MHz. Se qualquer métrica piorar,
10 MHz permanece como configuração de produção.

## Rollback

1. Desligar as duas placas.
2. Remover os seis sinais do enlace, mantendo as placas isoladas.
3. Gravar os builds normais, sem perfil DM1.
4. Confirmar nos logs: `enlace dual-MCU desabilitado por configuração`.
5. Não restaurar periféricos no head sem seguir o mapa de migração; rollback do
   enlace não autoriza contenção nos GPIO1/2/14/41/42.

DM1 só muda para `FEITO` após evidência de E0–E6 e soak. Build aprovado não é
evidência elétrica.

## Registro de bancada — 2026-06-19

Hardware identificado pelo esptool:

- main Waveshare, COM5, MAC `90:e5:b1:cc:3d:58`, PSRAM octal 16 MB/1,8 V;
- head Freenove, COM12, MAC `20:6e:f1:b2:3c:f4`, PSRAM octal 8 MB/3,3 V.

Evidência inicial em 10 MHz, `HEAD_RESET` desconectado:

| Gate | Resultado |
| --- | --- |
| E0 placas isoladas | aprovado pelo operador |
| E1 periféricos legados removidos | aprovado pelo operador |
| E2 GND comum | 0 V medido |
| E3 handshake/heartbeat | aprovado |
| Main | `READY`, handshake 535 ms |
| Head | `READY`, handshake 98 ms |
| Integridade | `invalid=0`, `timeout=0`, `spi_err=0` |
| Tráfego | TX/RX crescente nos dois sentidos |

Durante o primeiro ensaio foram corrigidos dois problemas de software:

1. O main ainda executava o boot monolítico e o display ocupava SPI2 antes do
   enlace. O perfil `NB_DM1_BENCH_PROFILE` passou a ignorar todo legado.
2. Timeout de 20 ms no slave corria em fase com o poll de 20 ms do master. A
   janela do slave foi ampliada para 100 ms.

Checkpoint após observação do operador por mais de 30 minutos:

- placas permaneceram energizadas, estáveis e sem aquecimento percebido;
- a abertura simultânea das portas CH343 reiniciou os dois MCUs, portanto os
  contadores acumulados anteriores não puderam ser coletados;
- após o reset simultâneo, ambos recuperaram `READY` automaticamente;
- main: handshake 535 ms, `invalid=0`, `timeout=0`, `spi_err=0`;
- head: handshake 40 ms, `invalid=0`, `timeout=0`, `spi_to=0`, `spi_err=0`;
- TX/RX voltou a crescer nos dois sentidos.

Este checkpoint valida boot e recuperação limpa.

Soak instrumentado encerrado após aproximadamente 8 h 02 min, ainda em
10 MHz e com `HEAD_RESET` desconectado:

| Métrica | Main | Head |
| --- | ---: | ---: |
| Estado | `READY` | `READY` |
| Uptime observado | 28.940 s | 28.938 s |
| TX | 115.761 | 109.192 |
| RX | 109.199 | 115.752 |
| Frames inválidos | 0 | 0 |
| Retries | 0 | 0 |
| ACK timeout | 0 | 0 |
| Erros SPI | 0 | 0 |
| Timeout SPI slave | — | 0 |
| Handshake inicial | 535 ms | 40 ms |

Durante o período, o operador confirmou estabilidade e ausência de aquecimento,
travamentos, resets visíveis ou comportamento anormal. A captura serial final
não reiniciou as placas e preservou os contadores acumulados.

Resultado do soak de 8 horas: **APROVADO em 10 MHz**.

Pendentes: fault injection E5 e `HEAD_RESET` E6.
