# NoiseBot — Hardware

## MCU

**Freenove ESP32-S3-WROOM CAM N16R8**

| Recurso           | Valor                                     |
| ----------------- | ----------------------------------------- |
| Cores             | 2× Xtensa LX7 @ 240MHz                    |
| SRAM              | 512KB                                     |
| PSRAM             | 8MB (octal SPI)                           |
| Flash             | 16MB                                      |
| USB               | OTG (nativo) + CP2102 (UART0 debug)       |
| Alimentação placa | 3.3V (regulado interno do 5V USB/externo) |

### Recursos de Periféricos Relevantes

| Periférico | Qtd disponível                          | Uso no NoiseBot                              |
| ---------- | --------------------------------------- | -------------------------------------------- |
| SPI        | 4 (SPI0/1 internos, SPI2/3 disponíveis) | SPI2: display                                |
| SDMMC      | 1                                       | SDMMC 1-bit: microSD (onboard)              |
| I2S        | 2                                       | I2S0: mic + speaker (full-duplex); I2S1: livre |
| UART       | 3                                       | UART0: debug, UART1: FE-TTLinker            |
| RMT        | 8 canais                                | 1 canal: WS2812                              |
| Touch      | 14 canais                               | T2 (GPIO 2): fita de cobre                  |
| I2C        | 2                                       | Reservado: câmera SCCB + IMU (adiados)      |
| LEDC/PWM   | 8 canais                                | Disponível (backlight caso ILI9342)         |

---

## Mapa de GPIOs

| GPIO  | Status            | Periférico / Função                                      |
| ----- | ----------------- | -------------------------------------------------------- |
| 0     | EVITAR            | Strapping: boot mode (LOW = download mode)               |
| 1     | EM USO            | I2S0 DIN — speaker MAX98357A TX (sacrifica T1)          |
| 2     | EM USO            | Touch T2 — fita de cobre                                |
| 3     | EM USO            | WS2812 RMT — 2 LEDs externos (T3/JTAG sacrificados)     |
| 4     | RESERVADO\_CAMERA | DVP SIOD (SDA câmera + I2C IMU futuro)                  |
| 5     | RESERVADO\_CAMERA | DVP SIOC (SCL câmera + I2C IMU futuro)                  |
| 6     | RESERVADO\_CAMERA | DVP VSYNC                                                |
| 7     | RESERVADO\_CAMERA | DVP HREF                                                 |
| 8     | RESERVADO\_CAMERA | DVP D2 (Y4)                                              |
| 9     | RESERVADO\_CAMERA | DVP D1 (Y3)                                              |
| 10    | RESERVADO\_CAMERA | DVP D3 (Y5)                                              |
| 11    | RESERVADO\_CAMERA | DVP D0 (Y2)                                              |
| 12    | RESERVADO\_CAMERA | DVP D4 (Y6)                                              |
| 13    | RESERVADO\_CAMERA | DVP PCLK                                                 |
| 14    | EM USO            | I2S0 SD — mic INMP441 RX (sacrifica T14)                |
| 15    | RESERVADO\_CAMERA | DVP XCLK                                                 |
| 16    | RESERVADO\_CAMERA | DVP D7 (Y9)                                              |
| 17    | RESERVADO\_CAMERA | DVP D6 (Y8)                                              |
| 18    | RESERVADO\_CAMERA | DVP D5 (Y7)                                              |
| 19    | EM USO ⚠          | UART1 RX ← TTLinker RX0 (USB D+, pulsos esporádicos WiFi)|
| 20    | EM USO            | UART1 TX → TTLinker TX1 (USB D-)                         |
| 21    | EM USO            | SPI2 MOSI — display ST7789                              |
| 22–25 | N/A               | Não existem no ESP32-S3                                  |
| 26–32 | INACESSÍVEL       | Octal PSRAM (SPI0/1 interno, N16R8)                     |
| 33    | AUSENTE           | Não existe no header da placa Freenove N16R8             |
| 34    | INACESSÍVEL       | Flash interno (N16R8)                                    |
| 35–37 | INACESSÍVEL       | PSRAM octal — presentes no header mas internamente usados|
| 38    | EM USO            | SDMMC CMD — microSD                                     |
| 39    | EM USO            | SDMMC CLK — microSD                                     |
| 40    | EM USO            | SDMMC DATA0 — microSD                                   |
| 41    | EM USO            | I2S0 BCLK — mic + speaker compartilhado                 |
| 42    | EM USO            | I2S0 LRCK — mic + speaker compartilhado                 |
| 43    | RESERVADO\_SYS    | UART0 TX — debug/programming                            |
| 44    | RESERVADO\_SYS    | UART0 RX — debug/programming                            |
| 45    | EM USO ⚠          | SPI2 DC — display ST7789 (strapping VDD\_SPI)           |
| 46    | EVITAR            | Strapping SDIO: pull-down interno corrompe upload UART  |
| 47    | EM USO            | SPI2 SCLK — display ST7789                              |
| 48    | RESERVADO\_SYS    | LED onboard azul Freenove — status visual, não repurpose |

---

## Periféricos Ativos

### Display — ST7789 2" (SPI2)

| Sinal | GPIO | Notas                                              |
| ----- | ---- | -------------------------------------------------- |
| SCLK  | 47   | SPI2 clock                                         |
| MOSI  | 21   | SPI2 data                                          |
| MISO  | —    | Não usado pelo ST7789 (sem leitura)                |
| CS    | GND  | Tied GND — display sempre selecionado              |
| DC    | 45   | Data/Command ⚠ strapping pin (VDD_SPI); ver nota  |
| RST   | —    | Software reset (sem pino físico neste módulo)      |
| BL    | —    | Sem backlight no módulo ST7789 atual               |

Frequência SPI: 60MHz (80MHz instável no bring-up). Testado e validado em hardware.

> ⚠ **GPIO 45 (DC) é strapping pin** (VDD\_SPI voltage). O estado ao ligar
> depende do driver LovyanGFX, que inicializa DC como OUTPUT HIGH antes do
> primeiro comando. Se houver glitch LOW no boot, adicionar pull-up 10kΩ.

### microSD (SDMMC 1-bit)

| Sinal | GPIO | Notas                       |
| ----- | ---- | --------------------------- |
| CLK   | 39   | SDMMC clock                 |
| CMD   | 38   | SDMMC command               |
| DATA0 | 40   | SDMMC data (1-bit mode)     |
| CS    | —    | Não usado em SDMMC (sem CS) |

Interface: **SDMMC** (não SPI). SD onboard da placa Freenove, 1-bit mode.
GPIO 39/40 também são pinos JTAG (TCK/TDO) — JTAG externo incompatível com SD ativo.

### WS2812 LEDs (RMT)

| Sinal | GPIO | Notas                        |
| ----- | ---- | ---------------------------- |
| DATA  | 3    | RMT canal 0, 2 LEDs em série |

Alimentação: 5V direto. Corrente máxima: ~120mA a 100% RGB (não usar 100% em operação normal).

> **GPIO 3** sacrifica Touch T3 e JTAG. Câmera DVP não usa GPIO 3 — seguro para uso permanente.
>
> **Por que não GPIO 19:** GPIO 19 = USB D+ do ESP32-S3. O stack WiFi usa
> `CONFIG_SOC_WIFI_PHY_NEEDS_USB_WORKAROUND` que reconfigura o bloco USB PHY ao
> conectar a um AP — isso contesta GPIO 19 com o RMT e trava as atualizações dos
> LEDs. GPIO 3 não tem essa restrição. GPIO 19 foi realocado para UART1 RX (servo),
> onde o risco de contenção com WiFi PHY é menor (UART tem maior tolerância a glitches).

### INMP441 — Microfone + MAX98357A — Amplificador (I2S0 full-duplex)

Mic (RX) e speaker (TX) **compartilham I2S0** em modo full-duplex.
BCLK e WS em GPIO 41/42 (sem função touch nem strapping).
GPIO 14 e 1 usados para dados — sacrificam TOUCH\_PAD\_NUM14 e T1.
**Restrição:** ambos operam a 16kHz (sample rate unificado).

| Sinal            | GPIO | Periférico        | Notas                             |
| ---------------- | ---- | ----------------- | --------------------------------- |
| BCLK (SCK)       | 41   | Mic + Speaker     | Compartilhado — sem função touch  |
| WS (LRCK)        | 42   | Mic + Speaker     | Compartilhado — sem função touch  |
| SD (DATA)        | 14   | Mic INMP441       | RX — sacrifica TOUCH\_PAD\_NUM14  |
| DIN              | 1    | Speaker MAX98357A | TX — sacrifica TOUCH\_PAD\_NUM1   |
| SD\_MODE         | —    | Speaker MAX98357A | Tied 3.3V (10kΩ) — sempre ativo  |
| GAIN             | —    | Speaker MAX98357A | Float = 9dB (15W@4Ω max)         |
| L/R              | GND  | Mic INMP441       | Canal esquerdo                    |

Controle de volume: divisão digital do sinal PCM (MAX98357A não tem I2C).
GPIO 48: LED onboard azul da placa Freenove — reservado para status visual, não repurposear.

### SCS0009 Servos (UART1 via FE-TTLinker)

| Sinal | GPIO | Notas                      |
| ----- | ---- | -------------------------- |
| TX  | 20   | UART1 TX → TTLinker TX1 (USB D-)                          |
| RX  | 19   | UART1 RX ← TTLinker RX0 (USB D+) ⚠ pulsos WiFi PHY       |

O FE-TTLinker é full-duplex no lado MCU — TX1 e RX0 são pinos separados.
A conversão para half-duplex ocorre internamente no barramento dos servos.

> **GPIO 33 ausente no header** da placa Freenove N16R8.
>
> **GPIO 19 como RX (entrada):** o `CONFIG_SOC_WIFI_PHY_NEEDS_USB_WORKAROUND`
> pode injetar pulsos esporádicos ao conectar a um AP. Como RX de UART, o
> checksum Feetech descarta pacotes corrompidos e o driver faz retry automático.
> Risco muito menor do que GPIO 19 como saída (RMT falhava completamente).

Baud rate: 1Mbps (padrão Feetech SCS0009).
IDs de servo: NECK\_PAN = 1, NECK\_TILT = 2.
Alimentação servos: 5V (linha separada com capacitor bulk obrigatório).

### Touch — Fita de Cobre (Touch Peripheral)

| Sinal    | GPIO | Notas                              |
| -------- | ---- | ---------------------------------- |
| TOUCH\_IN | 2   | Touch T2 — fita de cobre          |

Threshold calibrado em runtime (baseline × 1 + SENSITIVITY\_FACTOR).
SENSITIVITY\_FACTOR default: 0.2 (ajustar empiricamente com material final).

GPIOs touch disponíveis para expansão futura: GPIO 1 (T1, em uso como WS), GPIO 3 (T3, spare).

---

## Hardware Adiado (Pinos Reservados)

### OV2640 — Câmera (DVP, ADIADA)

A câmera OV2640 usa a interface DVP (parallel) com os pinos abaixo.
**ESTES PINOS NÃO PODEM SER REUSADOS.** Estão no header da placa E no conector FPC da câmera.
Confirmado pelo pinout oficial (docs/ESP32S3\_Pinout.png).

| Sinal DVP  | GPIO | Restrição                                       |
| ---------- | ---- | ----------------------------------------------- |
| D0 (Y2)    | 11   | Dado pixel — RESERVADO\_CAMERA                  |
| D1 (Y3)    | 9    | Dado pixel — RESERVADO\_CAMERA                  |
| D2 (Y4)    | 8    | Dado pixel — RESERVADO\_CAMERA                  |
| D3 (Y5)    | 10   | Dado pixel — RESERVADO\_CAMERA                  |
| D4 (Y6)    | 12   | Dado pixel — RESERVADO\_CAMERA                  |
| D5 (Y7)    | 18   | Dado pixel — RESERVADO\_CAMERA                  |
| D6 (Y8)    | 17   | Dado pixel — RESERVADO\_CAMERA                  |
| D7 (Y9)    | 16   | Dado pixel — RESERVADO\_CAMERA                  |
| PCLK       | 13   | Pixel clock — RESERVADO\_CAMERA                 |
| XCLK       | 15   | Clock saída para câmera — RESERVADO\_CAMERA     |
| VSYNC      | 6    | Sync vertical — RESERVADO\_CAMERA               |
| HREF       | 7    | Sync horizontal — RESERVADO\_CAMERA             |
| SIOD (SDA) | 4    | I2C câmera — compartilhável com IMU (etapa-8.x) |
| SIOC (SCL) | 5    | I2C câmera — compartilhável com IMU (etapa-8.x) |
| RESET      | —    | Tied 3.3V na placa (sem pino GPIO)              |
| PWDN       | —    | Tied GND na placa (sem pino GPIO)               |

> I2C da câmera (GPIO 4/5) pode ser compartilhado com MPU-6050 (0x68),
> bq25185 (0x6B) e MAX17048 (0x36) — endereços não colidem com OV2640 (0x3C).

Ao reativar a câmera: alocar 300KB de PSRAM para frame buffer (manter headroom desde já).

### MPU-6050 — IMU (I2C0, ADIADO)

| Sinal | GPIO | Notas      |
| ----- | ---- | ---------- |
| SDA   | TBD  | I2C0 data  |
| SCL   | TBD  | I2C0 clock |

Endereço: 0x68. Não inicializar I2C0 na fase inicial — reservar os pinos.

### LiPo + Gestão de Energia (ADIADO)

Componentes adiados: bq25185 (I2C0, 0x6B), MAX17048 (I2C0, 0x36), TPS61088 (boost).
Nenhum GPIO destes interfere com o hardware ativo. Sem restrições adicionais na fase inicial.

---

## Barramento 5V — Topologia de Alimentação

```
Fonte RPi4 (5V / 3A)
        │
        ├──[fio+]─── ESP32-S3 board (5V in → LDO 3.3V interno)
        │
        ├──[fio+]─── WS2812 × 2 (5V direto)
        │
        ├──[fio+]─── MAX98357A (5V)
        │
        ├──[fio+]─── FE-TTLinker (5V)
        │
        └──[fio+]─── ─[470µF 10V]─[100nF]─ SCS0009 × 2 (5V)
                                              ↑
                                    Capacitor bulk obrigatório
                                    (perto dos conectores dos servos)
        │
       GND comum (star ground — todos os componentes no mesmo ponto)
```

**Não alimentar servos pela USB-C da placa Freenove.** A USB-C tem limite de corrente insuficiente para servo + áudio simultâneos.

---

## Notas de Verificação em Bancada

Antes de começar Etapa 0.1, verificar fisicamente:

- [ ] Todos os GPIOs dos periféricos ativos identificados no schematic da placa
- [ ] Pinos DVP da câmera documentados e marcados como reservados
- [ ] Capacitor bulk instalado na linha dos servos
- [ ] GND comum conectado em todos os componentes
- [ ] Tensão 5V medida na entrada dos servos (sem carga): deve ser 4.9–5.1V
- [ ] Fita de cobre conectada ao GPIO de touch e testada por contato manual
