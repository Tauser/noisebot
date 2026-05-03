# Estudo de Reorganizacao de GPIOs

Este estudo consolida a pinagem atual do NoiseBot e avalia caminhos para ligar
todos os perifericos ativos sem depender de GPIOs que entram em conflito com o
WiFi/USB PHY do ESP32-S3.

## Problema

O par `GPIO19/GPIO20` pertence ao bloco USB nativo do ESP32-S3:

- `GPIO19`: USB D+
- `GPIO20`: USB D-

Em hardware, `GPIO19` ja apresentou falha quando usado para WS2812 com WiFi
ativo. Como o stack WiFi usa workaround que pode reconfigurar o USB PHY, esses
pinos nao devem ser tratados como GPIOs confiaveis para sinais criticos quando
WiFi estiver habilitado.

O FE-TTLinker/SCS0009 usa UART a 1 Mbps. Perder bytes ou receber glitches nesse
barramento pode causar timeout, leitura incorreta de status, perda de PING ou
falha em safety. Portanto, a solucao final deve evitar `GPIO19` e,
preferencialmente, tambem evitar `GPIO20`.

## Pinagem Atual

| GPIO | Uso atual / reserva | Observacao |
| ---- | ------------------- | ---------- |
| 0 | Evitar | Strapping boot mode |
| 1 | Speaker MAX98357A DIN | I2S0 TX |
| 2 | Touch | Fita de cobre, T2 |
| 3 | WS2812 | RMT, substituiu GPIO19 |
| 4 | I2C SDA / camera SIOD | Reservado camera, compartilhavel com sensores I2C |
| 5 | I2C SCL / camera SIOC | Reservado camera, compartilhavel com sensores I2C |
| 6 | Camera VSYNC | Reservado camera |
| 7 | Camera HREF | Reservado camera |
| 8 | Camera D2/Y4 | Reservado camera |
| 9 | Camera D1/Y3 | Reservado camera |
| 10 | Camera D3/Y5 | Reservado camera |
| 11 | Camera D0/Y2 | Reservado camera |
| 12 | Camera D4/Y6 | Reservado camera |
| 13 | Camera PCLK | Reservado camera |
| 14 | Mic INMP441 SD | I2S0 RX |
| 15 | Camera XCLK | Reservado camera |
| 16 | Camera D7/Y9 | Reservado camera |
| 17 | Camera D6/Y8 | Reservado camera |
| 18 | Camera D5/Y7 | Reservado camera |
| 19 | FE-TTLinker RX atual | USB D+, problema confirmado com WiFi |
| 20 | FE-TTLinker TX atual | USB D-, suspeito pelo mesmo bloco USB PHY |
| 21 | Display MOSI | SPI2 |
| 22-25 | N/A | Nao existem no ESP32-S3 |
| 26-32 | Inacessivel | PSRAM/flash internos |
| 33 | Ausente | Nao exposto no header Freenove N16R8 |
| 34 | Inacessivel | Flash interno |
| 35-37 | Inacessivel | PSRAM octal |
| 38 | microSD CMD | SDMMC |
| 39 | microSD CLK | SDMMC |
| 40 | microSD DATA0 | SDMMC |
| 41 | Audio BCLK | I2S0 compartilhado |
| 42 | Audio LRCK | I2S0 compartilhado |
| 43 | UART0 TX | Debug/programming |
| 44 | UART0 RX | Debug/programming |
| 45 | Display DC | Strapping VDD_SPI, ja validado |
| 46 | Evitar | Strapping SDIO, problematico com sinais idle-HIGH |
| 47 | Display SCLK | SPI2 |
| 48 | LED onboard | Reservado status/debug |

## Conclusao de Espaco

Com camera reservada, microSD ativo, display ativo, audio full-duplex, touch,
WS2812 e debug UART0, nao ha dois GPIOs livres e limpos no header para uma UART
dedicada do FE-TTLinker.

Isso significa que uma reorganizacao apenas por firmware nao resolve o produto
final se todos estes requisitos permanecerem simultaneos:

- WiFi ativo.
- Camera futura preservada.
- microSD onboard ativo.
- Debug/programming em UART0 `GPIO43/GPIO44`.
- FE-TTLinker usando UART full-duplex em dois fios.

E preciso escolher uma troca arquitetural.

## Opcoes Avaliadas

### Opcao A: Manter `GPIO20/19` para servo

Ligacao:

| ESP32-S3 | FE-TTLinker |
| -------- | ----------- |
| GPIO20 | RX |
| GPIO19 | TX |
| GND | GND |
| 5V | VCC |

Veredito: aceita apenas como teste offline. Nao recomendada para produto com
WiFi, porque `GPIO19` ja falhou em hardware com WiFi ativo.

### Opcao B: FE-TTLinker half-duplex em `GPIO20`

Ligacao proposta:

| ESP32-S3 | FE-TTLinker |
| -------- | ----------- |
| GPIO20 | TX/RX via circuito half-duplex |
| GND | GND |
| 5V | VCC |

Veredito: reduz o uso de `GPIO19`, mas ainda depende de `GPIO20`, que pertence
ao mesmo bloco USB PHY. Alem disso, precisa confirmar se o FE-TTLinker permite
half-duplex eletrico em um unico fio ou se exige TX/RX separados. Nao e a melhor
solucao de produto.

### Opcao C: Mover debug para USB CDC nativo e usar UART0 para servo

Ligacao proposta:

| ESP32-S3 | FE-TTLinker |
| -------- | ----------- |
| GPIO43 | RX do FE-TTLinker |
| GPIO44 | TX do FE-TTLinker |
| GND | GND |
| 5V | VCC |

Firmware:

- Console/debug passa para USB CDC/JTAG nativo do ESP32-S3.
- FE-TTLinker usa `UART_NUM_0` em `GPIO43/GPIO44`.
- Boot/programming nao deve depender do mesmo UART0 externo durante testes.

Veredito: melhor solucao se quisermos manter a placa atual, camera reservada,
microSD e WiFi. A troca e operacional: abrir mao de UART0 como console fisico e
usar USB CDC/JTAG para logs/debug.

### Opcao D: Liberar pinos da camera DVP

Exemplo: usar `GPIO16/GPIO17` ou `GPIO17/GPIO18` para UART servo.

Veredito: tecnicamente simples, mas viola a regra de projeto de nao realocar
pinos DVP fisicamente conectados. So deve ser considerada se a camera for
removida definitivamente do produto.

### Opcao E: Trocar o FE-TTLinker por interface nao-UART

Possibilidades:

- Controlador de servo via I2C, se existir modulo compativel com SCS0009.
- Controlador externo dedicado no barramento I2C ja existente `GPIO4/GPIO5`.
- Pequeno MCU ponte I2C -> SCS serial, deixando o ESP32-S3 falar I2C.

Veredito: melhor do ponto de vista de GPIO, porque I2C ja existe e pode ser
compartilhado. Custa hardware extra e firmware de ponte, mas preserva WiFi,
camera, SD, audio, display e debug sem briga de pino.

## Recomendacao

Para o NoiseBot na placa Freenove N16R8, a reorganizacao recomendada e:

| Periferico | GPIOs finais recomendados |
| ---------- | ------------------------- |
| Display ST7789 | SCLK `47`, MOSI `21`, DC `45`, CS em GND |
| microSD | CMD `38`, CLK `39`, DATA0 `40` |
| WS2812 | DATA `3` |
| Touch | `2` |
| INMP441 + MAX98357A | BCLK `41`, LRCK `42`, MIC SD `14`, SPK DIN `1` |
| I2C sensores/camera SCCB | SDA `4`, SCL `5` |
| Camera DVP | `6-13`, `15-18`, mais SCCB `4/5` |
| FE-TTLinker | Preferencial: UART0 `43/44`, com debug em USB CDC/JTAG |
| GPIO19/20 | Nao usar para sinais criticos com WiFi ativo |
| GPIO46 | Evitar |
| GPIO48 | LED onboard/status |

Esta opcao preserva todos os perifericos planejados e remove o servo do bloco
USB PHY. O custo e mover o canal de debug para USB CDC/JTAG.

## Plano de Validacao

1. Habilitar console/logs via USB CDC/JTAG no `sdkconfig`.
2. Alterar `NB_SERVO_UART_PORT` para `UART_NUM_0`.
3. Alterar `NB_SERVO_PIN_TX` para `43`.
4. Alterar `NB_SERVO_PIN_RX` para `44`.
5. Ligar FE-TTLinker cruzado: `GPIO43 -> RX`, `GPIO44 <- TX`.
6. Validar upload/programming com FE-TTLinker conectado.
7. Rodar PING dos servos ID 1 e 2 a 1 Mbps.
8. Ativar WiFi, conectar em AP e repetir PING/read position por pelo menos 10 min.
9. Validar audio, display, WS2812, touch e microSD simultaneamente.
10. So liberar movimento apos `motion_safety` verde.

## Decisao Aberta

Antes de alterar firmware, confirmar qual fluxo de debug sera usado:

- USB CDC/JTAG nativo como console principal.
- UART0 fisica dedicada ao FE-TTLinker.

Se essa decisao for aceita, `GPIO19/GPIO20` devem sair da pinagem de servo e
ficar marcados como proibidos para sinais criticos quando WiFi estiver ativo.
