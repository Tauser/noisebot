# NodeBot — Especificacao de Hardware

> Hardware congelado. Nenhum componente pode ser alterado salvo risco critico incontornavel documentado.

## MCU Principal

**Freenove ESP32-S3-WROOM CAM N16R8**

| Parametro        | Valor                                      |
|------------------|--------------------------------------------|
| CPU              | Xtensa LX7 dual-core @ 240 MHz            |
| SRAM interna     | 512 KB (fragmentada em bancos)             |
| Flash            | 16 MB (SPI Quad/Octal)                     |
| PSRAM            | 8 MB (Octal SPI, latencia ~30-80ns)        |
| Perifericos I2S  | 2x I2S (I2S0 e I2S1)                      |
| Perifericos SPI  | 3x SPI (SPI2 e SPI3 disponiveis para uso) |
| Perifericos UART | 3x UART                                    |
| Perifericos RMT  | 8 canais                                   |
| Touch            | 14 pinos touch capacitivo                  |
| Camera           | DVP interface (parallel) — OV2640 onboard |

### Restricao critica de GPIO

A camera OV2640 usa interface DVP paralela que ocupa permanentemente aproximadamente 12 pinos de GPIO (D0-D7, XCLK, PCLK, VSYNC, HREF). Esses pinos nao estao disponiveis para outros usos.

---

## Inventario de Perifericos

### Camera

| Atributo   | Valor                                       |
|------------|---------------------------------------------|
| Modelo     | OV2640                                      |
| Interface  | DVP paralela (integrada na placa Freenove)  |
| Resolucao  | ate 2MP (UXGA 1600x1200)                   |
| Alimentacao| 3.3V                                        |
| Nota       | Usa DMA de alta largura de banda            |

### Display

| Atributo   | Valor                     |
|------------|---------------------------|
| Modelo     | ST7789                    |
| Tamanho    | 2 polegadas               |
| Resolucao  | 240x320 pixels            |
| Interface  | SPI                       |
| Alimentacao| 3.3V                      |
| Framebuffer| ~150KB @ 16bpp — usar PSRAM |

### LEDs

| Atributo   | Valor                                         |
|------------|-----------------------------------------------|
| Modelo     | WS2812B                                       |
| Quantidade | 2 LEDs                                        |
| Interface  | GPIO via RMT (timing critico 1.25 µs)         |
| Alimentacao| 5V (verificar nivel logico — 3.3V geralmente aceito com HIGH > 0.7*Vcc) |

### Microfone

| Atributo       | Valor                               |
|----------------|-------------------------------------|
| Modelo         | INMP441                             |
| Interface      | I2S0 (half-duplex, entrada)         |
| Sample rate    | 16 kHz (voice) / ate 44.1 kHz     |
| Bit depth      | 32-bit (dados efetivos em 24-bit)   |
| Alimentacao    | 3.3V                                |
| Buffer DMA     | SRAM interna (pequeno) + PSRAM (ring buffer) |

### Amplificador / Speaker

| Atributo       | Valor                               |
|----------------|-------------------------------------|
| Modelo         | MAX98357A                           |
| Interface      | I2S1 (saida)                        |
| Potencia       | 3.2W @ 4Ω, 5V                      |
| Controle de gain | Pino GAIN (resistor ou GPIO)      |
| Alimentacao    | 5V                                  |
| Nota           | Consome corrente nao trivial — verificar impacto no power budget |

### Servos

| Atributo       | Valor                                         |
|----------------|-----------------------------------------------|
| Modelo         | SCS0009 (Feetech serial bus servo)            |
| Quantidade     | 2 unidades                                    |
| Interface      | Bus serial half-duplex via FE-TTLinker        |
| Protocolo      | SCServo (UART 1 Mbps, half-duplex)           |
| Alimentacao    | 4.5V - 6V (alimentado pelo boost 5V)          |
| Corrente pico  | ~1.5A por servo em stall                      |
| Features       | Posicao, temperatura, load, error — acessiveis por registro |
| Conversor      | FE-TTLinker (full-duplex UART para half-duplex serial bus) |

### IMU

| Atributo   | Valor                              |
|------------|------------------------------------|
| Modelo     | MPU-6050                           |
| Interface  | I2C (endereco 0x68 ou 0x69)        |
| Features   | Acelerometro 3-eixos + Giroscopio 3-eixos |
| DMP        | Processador de movimento onboard   |
| Alimentacao| 3.3V                               |
| Clock I2C  | 400 kHz (Fast Mode)                |

### Touch

| Atributo   | Valor                                     |
|------------|-------------------------------------------|
| Tipo       | Capacitivo via periférico interno ESP32-S3 |
| Eletrodo   | Fita de cobre                             |
| Interface  | Pinos touch do ESP32-S3                   |
| Calibracao | Baseline persistida em NVS               |

### Fuel Gauge

| Atributo   | Valor                                     |
|------------|-------------------------------------------|
| Modelo     | MAX17048                                  |
| Interface  | I2C (endereco 0x36)                       |
| Medicao    | SoC (%), tensao (mV), taxa de descarga    |
| Algoritmo  | ModelGauge (voltage-based + modelagem)    |
| Alimentacao| 3.3V                                      |
| Nota       | Precisa de aprendizado de ciclo para precisao maxima |

### Carregador

| Atributo   | Valor                                                 |
|------------|-------------------------------------------------------|
| Modelo     | Adafruit bq25185                                      |
| Interface  | I2C                                                   |
| Fontes     | USB / DC / Solar                                      |
| Celula     | LiPo/LiIon 1S                                        |
| Registradores | Status de carga, estado de fault, OVP, OCP, NTC  |
| Alimentacao entrada | 5V USB ou DC                               |

### Boost Converter

| Atributo       | Valor                                       |
|----------------|---------------------------------------------|
| Modelo         | TPS61088                                    |
| Saida          | 5V                                          |
| Corrente maxima| 10A                                         |
| Carga          | Servos SCS0009 + possivelmente WS2812       |
| Nota           | Gera ripple sob carga variavel — medir com osciloscópio antes de integrar sensores analogicos |

### Bateria

| Atributo   | Valor                                  |
|------------|----------------------------------------|
| Tipo       | LiPo 1S                                |
| Capacidade | 3000 mAh                               |
| Tensao nominal | 3.7V                              |
| Tensao maxima  | 4.2V                              |
| Tensao minima segura | 3.0V (cutoff de sistema)    |
| Tensao de dano | < 2.5V (dano irreversivel a celula) |

---

## Diagrama Textual do Power Path

```
USB / DC / Solar
       |
       v
  [bq25185 Charger] ---(I2C status)--- ESP32-S3
       |
       v
  [LiPo 1S 3000mAh] ---(I2C SoC/V)--- [MAX17048]
       |
       |----> [TPS61088 Boost 5V/10A] --> Servos SCS0009
       |                              --> MAX98357A (se 5V)
       |                              --> WS2812 (se 5V)
       |
       v
  [LDO 3.3V (onboard Freenove)]
       |
       v
  ESP32-S3, ST7789, INMP441, MPU-6050, MAX17048, bq25185
```

---

## Mapa de Barramentos

### I2C (compartilhado, 400 kHz)

| Dispositivo | Endereco | Notas                          |
|-------------|----------|--------------------------------|
| MPU-6050    | 0x68     | AD0 = GND                      |
| MAX17048    | 0x36     | Fixo                           |
| bq25185     | 0x6B     | Verificar datasheet Adafruit   |

### SPI

| Dispositivo | Barramento | CS    | Clock max |
|-------------|------------|-------|-----------|
| ST7789      | SPI2       | GPIO dedicado | 40-80 MHz |
| microSD     | SPI2 ou SPI3 | GPIO dedicado | 20-40 MHz |

Nota: ST7789 e microSD podem compartilhar barramento SPI com CS separados, ou usar barramentos distintos dependendo da pinagem disponivel.

### I2S

| Canal | Dispositivo  | Direcao  |
|-------|--------------|----------|
| I2S0  | INMP441      | Entrada  |
| I2S1  | MAX98357A    | Saida    |

### UART

| Canal  | Dispositivo    | Notas                     |
|--------|----------------|---------------------------|
| UART0  | Debug (USB)    | 921600 baud               |
| UART1  | FE-TTLinker    | Half-duplex via GPIO dedicado |

### RMT

| Canal | Dispositivo | Notas                                |
|-------|-------------|--------------------------------------|
| RMT0  | WS2812 x2  | Canal dedicado, nao compartilhar     |

### Touch (ESP32-S3 interno)

| Pino  | Uso          |
|-------|--------------|
| T0-T13 | Disponiveis |
| T?    | Fita de cobre (a definir na pinagem final) |

---

## Tabela de Pressao de Recursos

| Recurso  | Pressao     | Competidores principais                     |
|----------|-------------|---------------------------------------------|
| I2S      | Alta        | INMP441 (I2S0) + MAX98357A (I2S1)          |
| DMA      | Critica     | Camera DVP + I2S + SPI                      |
| PSRAM    | Alta        | Framebuffer display + audio ring buffer + camera frames |
| SRAM     | Critica     | DMA buffers (nao podem estar em PSRAM) + FreeRTOS stacks |
| SPI      | Media-alta  | Display + microSD + Flash interna (shared)  |
| UART     | Media       | Debug + FE-TTLinker                         |
| GPIO     | Alta        | DVP da camera trava ~12 pinos               |
| CPU Core0| Alta        | WiFi/BT stack (se ativo) + protocolo tasks  |
| CPU Core1| Media-alta  | Todas as tasks de aplicacao                 |

---

## Notas de Integracao

1. **Camera vs. Audio simultaneos:** OV2640 DVP usa DMA de alta banda. Testar camera + I2S juntos antes de integrar comportamento — conflito de DMA e real.

2. **Boost ripple:** TPS61088 sob carga variavel de servos pode gerar ripple no 5V que afeta leituras I2C/ADC. Medir com osciloscópio antes de integrar sensores.

3. **WS2812 nivel logico:** Verificar que 3.3V HIGH e aceito. A maioria dos WS2812B aceita se Vih > 0.7*Vcc (3.5V = limiar, mas na pratica 3.3V funciona em muitos casos). Se houver problemas, usar level shifter.

4. **SCS0009 corrente de pico:** Dois servos em aceleracao simultanea podem puxar 2-3A do boost. Verificar que LiPo consegue fornecer sem colapso de tensao.
