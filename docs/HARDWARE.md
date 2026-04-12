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

| Periférico | Qtd disponível                          | Uso no NoiseBot                      |
| ---------- | --------------------------------------- | ------------------------------------ |
| SPI        | 4 (SPI0/1 internos, SPI2/3 disponíveis) | SPI2: display, SPI3: microSD         |
| I2S        | 2                                       | I2S0: mic, I2S1: speaker             |
| UART       | 3                                       | UART0: debug, UART1: FE-TTLinker     |
| RMT        | 8 canais                                | 1 canal: WS2812                      |
| Touch      | 14 canais                               | 1+ canais: fita de cobre             |
| I2C        | 2                                       | Reservado: IMU, fuel gauge (adiados) |
| LEDC/PWM   | 8 canais                                | 1 canal: backlight display           |

---

## Periféricos Ativos

### Display — ST7789 2" (SPI2)

| Sinal | GPIO | Notas                                     |
| ----- | ---- | ----------------------------------------- |
| SCLK  | TBD  | SPI2 clock                                |
| MOSI  | TBD  | SPI2 data                                 |
| MISO  | TBD  | Não usado pelo ST7789, mas pino reservado |
| CS    | TBD  | Chip select display                       |
| DC    | TBD  | Data/Command                              |
| RST   | TBD  | Reset display (ativo baixo)               |
| BL    | TBD  | Backlight (LEDC PWM)                      |

Frequência SPI: 40MHz conservador no bring-up; testar 80MHz após estabilização.

### microSD (SPI3)

| Sinal | GPIO | Notas          |
| ----- | ---- | -------------- |
| CLK   | TBD  | SPI3 clock     |
| MOSI  | TBD  | SPI3 data out  |
| MISO  | TBD  | SPI3 data in   |
| CS    | TBD  | Chip select SD |

O microSD da placa Freenove está onboard. Confirmar pinos com schematic da placa.
Bus SPI3 dedicado ao SD (não compartilhar com display para evitar arbitragem).

### WS2812 LEDs (RMT)

| Sinal | GPIO | Notas                        |
| ----- | ---- | ---------------------------- |
| DATA  | TBD  | RMT canal 0, 2 LEDs em série |

Alimentação: 5V direto. Corrente máxima: ~120mA a 100% RGB (não usar 100% em operação normal).

### INMP441 — Microfone (I2S0)

| Sinal     | GPIO | Notas                       |
| --------- | ---- | --------------------------- |
| WS (LRCK) | TBD  | Word select                 |
| SCK (BCK) | TBD  | Bit clock                   |
| SD (DATA) | TBD  | Saída serial do mic         |
| L/R       | GND  | Canal esquerdo (LOW = left) |

Frequência: 16kHz, 32 bits por sample (24 bits úteis, justificados à esquerda).

### MAX98357A — Amplificador (I2S1)

| Sinal    | GPIO | Notas                                   |
| -------- | ---- | --------------------------------------- |
| BCLK     | TBD  | Bit clock                               |
| LRC (WS) | TBD  | Left/right clock                        |
| DIN      | TBD  | Dado serial                             |
| SD_MODE  | TBD  | HIGH=ativo, LOW=shutdown (zero consumo) |
| GAIN     | —    | Hardware (resistor/float define ganho)  |

Configuração de ganho padrão: pino GAIN flutuante = 9dB (15W@4Ω max).
Controle de volume: via divisão digital do sinal PCM — MAX98357A não tem controle I2C.

### SCS0009 Servos (UART1 via FE-TTLinker)

| Sinal | GPIO | Notas                     |
| ----- | ---- | ------------------------- |
| TX    | TBD  | UART1 TX → FE-TTLinker RX |
| RX    | TBD  | UART1 RX ← FE-TTLinker TX |

Baud rate: confirmar com datasheet SCS0009 (padrão Feetech: 1Mbps, mas configurável).
IDs de servo: NECK_PAN = 1, NECK_TILT = 2.
Alimentação servos: 5V (linha separada com capacitor bulk obrigatório).

### Touch — Fita de Cobre (Touch Peripheral)

| Sinal    | GPIO | Notas                          |
| -------- | ---- | ------------------------------ |
| TOUCH_IN | TBD  | GPIO com função touch ESP32-S3 |

Threshold calibrado em runtime (baseline × 1 + SENSITIVITY_FACTOR).
SENSITIVITY_FACTOR default: 0.2 (ajustar empiricamente com material final).

---

## Hardware Adiado (Pinos Reservados)

### OV2640 — Câmera (DVP, ADIADA)

A câmera OV2640 usa a interface DVP (parallel) com os pinos abaixo.
**ESTES PINOS NÃO PODEM SER REUSADOS.** Estão fisicamente conectados na placa Freenove.

| Sinal DVP  | GPIO          | Restrição                     |
| ---------- | ------------- | ----------------------------- |
| D0–D7      | TBD (8 pinos) | Dados de pixel — RESERVADOS   |
| XCLK       | TBD           | Clock para câmera — RESERVADO |
| PCLK       | TBD           | Pixel clock — RESERVADO       |
| VSYNC      | TBD           | Sync vertical — RESERVADO     |
| HREF       | TBD           | Sync horizontal — RESERVADO   |
| SIOD (SDA) | TBD           | I2C câmera — RESERVADO        |
| SIOC (SCL) | TBD           | I2C câmera — RESERVADO        |
| RESET      | TBD           | Reset câmera — RESERVADO      |
| PWDN       | TBD           | Power down câmera             |

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
