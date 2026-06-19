# NoiseBot — Mapa de GPIO Dual-MCU

Mapa de pinos das duas placas seguindo o `DUAL_MCU_ARCHITECTURE_PLAN.md`.
Cobre função/direção, periférico ESP-IDF, pinos fixos/reservados, strapping/
USB/JTAG/boot, barramento compartilhado, tensão/pull, exposição no header e os
sinais SPI entre placas.

> **Status:** proposta de alocação. O pinout da **main (Waveshare)** ainda é
> *pendente de bancada* (HARDWARE.md). O da **head (Freenove)** reaproveita o
> mapa já validado, liberando o legado de áudio/servo/LED/touch.

> **Regra de ativação:** este documento descreve o estado-alvo. Definir os
> macros não autoriza energizar o enlace. O firmware monolítico atual ainda
> dirige GPIO1/2/14/41/42 da Freenove; esses fios devem ser removidos e os
> respectivos drivers desabilitados antes de conectar o SPI entre placas.

## Fontes confirmadas (placa, não só chip)

- Main: módulo **ESP32-S3-WROOM-2-N32R16V** (flash 32MB *octal* OPI + 16MB PSRAM).
  Esquemático Waveshare ESP32-S3-DEV-KIT (mesma PCB N8R8/N16R8/N32R16V) e tabela
  de comparação oficial. **Diferença do N32R16V:** SPI flash/PSRAM operam a 1.8V,
  logo **IO47/IO48 (domínio VDD_SPI) têm nível alto de apenas 1.8V**.
- Head: Freenove ESP32-S3-WROOM-1 CAM N16R8, conforme `HARDWARE.md` /
  `nb_hw_config.h` já validados em bancada.

Links:

- https://docs.waveshare.com/ESP32-S3-DEV-KIT-N8R8
- https://files.waveshare.com/wiki/ESP32-S3-DEV-KIT-N8R8/ESP32-S3-DEV-KIT-N8R8-schematic.pdf
- https://www.circuitstate.com/pinouts/waveshare-esp32-s3-dev-kit-nxr8-wi-fi-development-board-pinout-diagram-arduino-reference/

## Armadilhas específicas da placa Waveshare (confirmadas no esquemático)

1. **GPIO38 = WS2812B RGB onboard** (`RGB_CTRL`, via R5 0Ω). Ocupado pela placa —
   usar como LED de status ou deixar livre; não é um GPIO "limpo".
2. **GPIO35/36/37 expostos no header P2 porém NC** no módulo (PSRAM octal).
   Estão no conector mas **não funcionam** — não derivar disponibilidade do chip.
3. **GPIO33/34 não são expostos** no header desta placa (e no WROOM-2 entram no
   flash octal). Indisponíveis.
4. **GPIO0** = botão BOOT/FLASH (S1) + circuito auto-program (DTR/RTS→EN/IO0).
5. **EN/CHIP_PU** = botão RST (S2) + auto-program. Exposto no header P2 — é o
   alvo do `HEAD_RESET` quando esta placa for *head*; na *main* é a própria reset.
6. **GPIO43/44 (U0TXD/U0RXD)** ligados ao CH343 (USB-UART) — console.
7. **GPIO19/20** = USB D-/D+ nativos, roteados ao hub CH334F.
8. **GPIO45/46** strapping (45 = VDD_SPI; 46 = ROM boot msg). No N32R16V o GPIO45
   chaveia o domínio 1.8V.

---

## MAIN — Waveshare ESP32-S3 N32R16V (controlador do robô)

Direção: `→` saída do MCU, `←` entrada no MCU, `↔` bidirecional, `A` analógico.

| GPIO | Função (dir.) | Periférico ESP-IDF | Fixo/Reservado | Strapping/USB/JTAG/Boot | Barramento | Tensão / Pull | Header |
| --- | --- | --- | --- | --- | --- | --- | --- |
| EN | Reset (←) | — | Botão RST + auto-program | Reset de chip | — | 3.3V, PU | P2 |
| 0 | BOOT (←) | — | **Não usar** | Strapping boot; botão FLASH | — | 3.3V, weak PU | P1 |
| 1 | Reserva | `adc_oneshot`/`touch` | — | — | — | 3.3V | P1 |
| 2 | Touch corporal (←) | `touch_sensor` (TOUCH2) | — | — | — | 3.3V, sensor cap. | P1 |
| 3 | Reserva | — | Evitar p/ crítico | Strapping (JTAG sel), floating | — | 3.3V | P1 |
| 4 | I2C SDA (↔) | `i2c_master` (I2C0) | — | — | **I2C sensores** | 3.3V, PU ext. 4.7k | P1 |
| 5 | I2C SCL (↔) | `i2c_master` (I2C0) | — | — | **I2C sensores** | 3.3V, PU ext. 4.7k | P1 |
| 6 | Reserva | — | — | — | — | 3.3V | P1 |
| 7 | Monitor 5V (A) | `adc_oneshot` ADC1_CH6 | — | — | — | divisor 68k/56k → ≤3.1V | P1 |
| 8 | `HEAD_RESET` (→) | `gpio` | → pino EN do head | — | Link (controle) | 3.3V | P1 |
| 9 | Reserva | — | — | FSPIHD | — | 3.3V | P1 |
| 10 | `LINK_CS` (→) | `spi_master` SPI2 | — | FSPICS0 (IO MUX) | **Link SPI** | 3.3V | P1 |
| 11 | `LINK_MOSI` (→) | `spi_master` SPI2 | — | FSPID (IO MUX) | **Link SPI** | 3.3V | P1 |
| 12 | `LINK_SCLK` (→) | `spi_master` SPI2 | — | FSPICLK (IO MUX) | **Link SPI** | 3.3V | P1 |
| 13 | `LINK_MISO` (←) | `spi_master` SPI2 | — | FSPIQ (IO MUX) | **Link SPI** | 3.3V | P1 |
| 14 | `HEAD_IRQ` (←) | `gpio` ISR | — | FSPIWP (usado como GPIO) | Link (evento) | 3.3V | P1 |
| 15 | Reserva | — | — | — | — | 3.3V | P1 |
| 16 | Reserva | — | — | — | — | 3.3V | P1 |
| 17 | Servo UART1 TX (→) | `uart` UART1 | — | — | **Servo (FE-TTLinker)** | 3.3V | P1 |
| 18 | Servo UART1 RX (←) | `uart` UART1 | — | — | **Servo (FE-TTLinker)** | 3.3V | P1 |
| 19 | USB D- (↔) | `usb_serial_jtag` | Reservado USB | USB nativo / bridge CDC | USB | — | P2 |
| 20 | USB D+ (↔) | `usb_serial_jtag` | Reservado USB | USB nativo / bridge CDC | USB | — | P2 |
| 21 | WS2812 LEDs (→) | `rmt_tx` + `led_strip` | — | — | — | 3.3V → level-shift 5V | P2 |
| 35–37 | — | — | **NC (PSRAM octal)** | Exposto no header, inoperante | — | — | P2 (inútil) |
| 38 | LED RGB onboard (→) | `rmt_tx` | **WS2812 da placa** | — | — | 3.3V | P2 |
| 39 | Áudio MIC SD/RX (←) | `i2s_std` I2S0 | — | JTAG MTCK (perde JTAG) | **Áudio I2S0** | 3.3V | P2 |
| 40 | Áudio BCLK (→) | `i2s_std` I2S0 | — | JTAG MTDO | **Áudio I2S0** | 3.3V | P2 |
| 41 | Áudio WS/LRCK (→) | `i2s_std` I2S0 | — | JTAG MTDI | **Áudio I2S0** | 3.3V | P2 |
| 42 | Áudio SPK DIN/TX (→) | `i2s_std` I2S0 | — | JTAG MTMS | **Áudio I2S0** | 3.3V | P2 |
| 43 | Console TX (→) | UART0 / `usb_serial_jtag` | CH343 USB-UART | Programming | — | 3.3V | P2 |
| 44 | Console RX (←) | UART0 / `usb_serial_jtag` | CH343 USB-UART | Programming | — | 3.3V | P2 |
| 45 | — | — | Evitar | Strapping VDD_SPI (1.8V) | — | 1.8V dom. | P2 |
| 46 | — | — | Evitar | Strapping ROM boot msg | — | 3.3V, weak PD | P2 |
| 47 | — | — | **Não usar p/ lógica 3.3V** | VDD_SPI domain | — | **1.8V** | P2 |
| 48 | — | — | **Não usar p/ lógica 3.3V** | VDD_SPI domain | — | **1.8V** | P2 |

Áudio INMP441 (RX) + MAX98357A (TX) em **I2S0 full-duplex**, 16 kHz; BCLK/WS
compartilhados. Servo a 1 Mbps via FE-TTLinker (TX/RX separados lado MCU). O
servo **sai do par USB (19/20)** — encerrando a antiga contenção entre
WiFi e USB-PHY.

GPIOs realmente disponíveis (limpos) na main: **1, 6, 9, 15, 16** sobrando após a
alocação acima — folga confortável para expansão.

---

## HEAD — Freenove ESP32-S3 CAM N16R8 (multimídia local)

Mantém display, câmera, SD e console; recebe o **link SPI3 em modo slave** nos
pinos liberados pelo legado quando áudio/servo/LED/touch corporal forem
recabeados para a main.

| GPIO | Função (dir.) | Periférico ESP-IDF | Fixo/Reservado | Strapping/USB/JTAG/Boot | Barramento | Tensão / Pull | Header |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | BOOT (←) | — | Não usar | Strapping boot | — | 3.3V, PU | sim |
| 1 | `LINK_CS` (←) | `spi_slave` SPI3 | — | — | **Link SPI** | 3.3V | sim |
| 2 | `HEAD_IRQ` (→) | `gpio` | → main GPIO14 | — | Link (evento) | 3.3V | sim |
| 3 | Touchscreen INT (←) futuro | `gpio` ISR | Livre p/ F3 | Strapping (JTAG sel) | — | 3.3V | sim |
| 4 | Câmera SIOD / I2C SDA (↔) | `i2c_master` + SCCB | **DVP fixo** | — | **I2C câmera + touch** | 3.3V, PU | sim |
| 5 | Câmera SIOC / I2C SCL (↔) | `i2c_master` + SCCB | **DVP fixo** | — | **I2C câmera + touch** | 3.3V, PU | sim |
| 6 | Câmera VSYNC (←) | `esp_cam`/DVP | **DVP fixo** | — | Câmera | 3.3V | sim |
| 7 | Câmera HREF (←) | `esp_cam`/DVP | **DVP fixo** | — | Câmera | 3.3V | sim |
| 8 | Câmera D2 (←) | `esp_cam`/DVP | **DVP fixo** | — | Câmera | 3.3V | sim |
| 9 | Câmera D1 (←) | `esp_cam`/DVP | **DVP fixo** | — | Câmera | 3.3V | sim |
| 10 | Câmera D3 (←) | `esp_cam`/DVP | **DVP fixo** | — | Câmera | 3.3V | sim |
| 11 | Câmera D0 (←) | `esp_cam`/DVP | **DVP fixo** | — | Câmera | 3.3V | sim |
| 12 | Câmera D4 (←) | `esp_cam`/DVP | **DVP fixo** | — | Câmera | 3.3V | sim |
| 13 | Câmera PCLK (←) | `esp_cam`/DVP | **DVP fixo** | — | Câmera | 3.3V | sim |
| 14 | `LINK_MISO` (→) | `spi_slave` SPI3 | — | — | **Link SPI** | 3.3V | sim |
| 15 | Câmera XCLK (→) | `esp_cam`/DVP | **DVP fixo** | — | Câmera | 3.3V | sim |
| 16 | Câmera D7 (←) | `esp_cam`/DVP | **DVP fixo** | — | Câmera | 3.3V | sim |
| 17 | Câmera D6 (←) | `esp_cam`/DVP | **DVP fixo** | — | Câmera | 3.3V | sim |
| 18 | Câmera D5 (←) | `esp_cam`/DVP | **DVP fixo** | — | Câmera | 3.3V | sim |
| 19 | USB D+ (↔) | `usb_serial_jtag` | Reservado USB | USB nativo | USB | — | sim |
| 20 | USB D- (↔) | `usb_serial_jtag` | Reservado USB | USB nativo | USB | — | sim |
| 21 | Display MOSI (→) | `esp_lcd`/LovyanGFX SPI2 | — | — | **Display SPI2** | 3.3V | sim |
| 26–37 | — | — | PSRAM/Flash | 35–37 no header mas NC | — | — | não úteis |
| 38 | SD CMD (↔) | `sdmmc` | onboard | — | **microSD** | 3.3V, PU | sim |
| 39 | SD CLK (→) | `sdmmc` | onboard | JTAG TCK | **microSD** | 3.3V | sim |
| 40 | SD DATA0 (↔) | `sdmmc` | onboard | JTAG TDO | **microSD** | 3.3V, PU | sim |
| 41 | `LINK_SCLK` (←) | `spi_slave` SPI3 | — | — | **Link SPI** | 3.3V | sim |
| 42 | `LINK_MOSI` (←) | `spi_slave` SPI3 | — | — | **Link SPI** | 3.3V | sim |
| 43 | Console TX (→) | UART0 | CP2102 debug | Programming | — | 3.3V | sim |
| 44 | Console RX (←) | UART0 | CP2102 debug | Programming | — | 3.3V | sim |
| 45 | Display DC (→) | `esp_lcd`/LovyanGFX | — | Strapping VDD_SPI | Display SPI2 | 3.3V (PU 10k rec.) | sim |
| 46 | — | — | Evitar | Strapping SDIO | — | 3.3V, weak PD | sim |
| 47 | Display SCLK (→) | `esp_lcd`/LovyanGFX | — | — | Display SPI2 | 3.3V | sim |
| 48 | LED status onboard (→) | `gpio` | LED azul placa | — | — | 3.3V | sim |

Display ST7789 em **SPI2 master**; CS atado a GND. Link em **SPI3 slave** (SPI2
ocupado), passando pelo GPIO matrix — começar a 10 MHz, cabos curtos. Touchscreen
F3 piggyback no I2C da câmera (4/5; endereço não colide com OV2640 0x3C), INT no
GPIO3.

---

## Enlace inter-MCU (6 sinais + GND)

Main = SPI master (SPI2, pinos IO MUX nativos FSPI). Head = SPI slave (SPI3).

| Sinal | Dir. | Main (Waveshare) | Head (Freenove) | Periférico |
| --- | --- | --- | --- | --- |
| `LINK_SCLK` | main → head | GPIO12 (FSPICLK) | GPIO41 | spi_master / spi_slave |
| `LINK_MOSI` | main → head | GPIO11 (FSPID) | GPIO42 | spi_master / spi_slave |
| `LINK_MISO` | head → main | GPIO13 (FSPIQ) | GPIO14 | spi_master / spi_slave |
| `LINK_CS` | main → head | GPIO10 (FSPICS0) | GPIO1 | spi_master / spi_slave |
| `HEAD_IRQ` | head → main | GPIO14 (←, ISR) | GPIO2 (→) | gpio |
| `HEAD_RESET` | main → head | GPIO8 (→) | pino **EN** | gpio → reset HW |

GND comum, retorno próximo. 10 MHz no bring-up, promover a 20 MHz após teste de
integridade (seção 3 do plano). SPI ISR só sinaliza filas; parsing/CRC/dispatch
em tasks.

---

## Pendências de bancada

1. Confirmar a silk/variante exata da placa Waveshare em mãos (o esquemático
   público é do N8R8/N16R8; o N32R16V troca o módulo e leva 47/48 a 1.8V).
2. Validar `HEAD_RESET` no pino EN do head como reset de hardware (com rate limit).
3. Definir entrada ADC de 5V na main (GPIO7 proposto) e o divisor 68k/56k.
4. Sequenciamento: os pinos do link no head (1,2,14,41,42) só ficam limpos após
   um gate físico anterior ao bring-up DM1: recabeamento de áudio, servo, LED e
   touch corporal para a main, seguido de boot do head sem drivers legados.
   F2–F4 migram display/touchscreen/câmera e não liberam esses GPIOs.
5. Antes de unir as placas, medir com multímetro/osciloscópio que nenhum dos
   cinco GPIOs do head está sendo dirigido pelo firmware legado.
