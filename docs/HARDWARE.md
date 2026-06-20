# NoiseBot — Hardware

## Arquitetura dual-MCU em migração

- Waveshare ESP32-S3 N32R16: controlador principal de comportamento, áudio,
  rede, safety, servos, LEDs, touch corporal e sensores.
- Freenove ESP32-S3-WROOM CAM N16R8: controlador de cabeça para display,
  touchscreen, câmera e microSD único.

O mapa abaixo documenta a placa Freenove no papel de **head-controller**. As
ligações antigas de áudio, servos, LEDs e touch corporal nessa placa descrevem
o baseline monolítico e não o estado final. Não devem ser copiadas para o novo
firmware head.

O mapa definitivo da Waveshare será fechado somente após conferência do modelo
exato e validação elétrica em bancada. Até isso ocorrer, nenhum GPIO de áudio,
servo, LED, sensores ou enlace SPI da main é considerado aprovado. Ver
`DUAL_MCU_ARCHITECTURE_PLAN.md`. A proposta pino a pino, incluindo direção,
níveis elétricos e exposição nos headers, está em `GPIO_DUAL_MCU.md`.

Restrições confirmadas para a variante Waveshare N32R16V:

- GPIO38 possui WS2812 onboard e não é GPIO limpo;
- GPIO35/36/37 aparecem no header, mas são NC no módulo;
- GPIO47/48 têm high-level de 1,8V no domínio VDD_SPI e são proibidos para
  lógica 3,3V;
- GPIO19/20 permanecem reservados ao USB nativo;
- GPIO43/44 permanecem reservados ao console/programação.

O enlace físico só pode ser conectado após remover da Freenove as ligações
legadas de áudio/servo/LED/touch e confirmar que GPIO1/2/14/41/42 não estão
sendo dirigidos pelo firmware monolítico.

### Matriz de propriedade final

| Domínio | Main — Waveshare | Head — Freenove |
| --- | --- | --- |
| Safety, servos, comportamento | Autoridade e hardware | Proibido |
| Áudio I2S, wake, VAD | Autoridade e hardware | Proibido |
| LEDs e touch corporal | Autoridade e hardware | Proibido |
| Display e touchscreen | Intenção semântica | Autoridade e hardware |
| OV2640 | Solicita métricas/JPEG | DVP, captura e preview |
| microSD | Cliente assíncrono | Único mount FATFS/SDMMC |
| Tempo oficial | Fonte canônica | Relógio local sincronizado |

## MCU do head

**Freenove ESP32-S3-WROOM CAM N16R8**

| Recurso           | Valor                                     |
| ----------------- | ----------------------------------------- |
| Cores             | 2× Xtensa LX7 @ 240MHz                    |
| SRAM              | 512KB                                     |
| PSRAM             | 8MB (octal SPI)                           |
| Flash             | 16MB                                      |
| USB               | OTG (nativo) + CP2102 (UART0 debug)       |
| Alimentação placa | 3.3V (regulado interno do 5V USB/externo) |

### Recursos de Periféricos Relevantes do head

| Periférico | Qtd disponível                          | Uso no NoiseBot                              |
| ---------- | --------------------------------------- | -------------------------------------------- |
| SPI        | 4 (SPI0/1 internos, SPI2/3 disponíveis) | SPI2: display                                |
| SDMMC      | 1                                       | SDMMC 1-bit: microSD (onboard)              |
| I2S        | 2                                       | Livres no estado final do head               |
| UART       | 3                                       | UART0: debug; demais livres                  |
| RMT        | 8 canais                                | Livre no estado final do head                |
| Touch      | 14 canais                               | Touchscreen futuro usa contrato próprio      |
| I2C        | 2                                       | SCCB da câmera; touchscreen se compatível    |
| LEDC/PWM   | 8 canais                                | Disponível (backlight caso ILI9342)         |

---

## Mapa de GPIOs da Freenove

As marcações de áudio/servo/LED/touch abaixo são **legado monolítico** até a
migração física. No estado final só display, câmera, SD, enlace inter-MCU,
console e touchscreen podem permanecer na Freenove.

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
| 19    | EM USO ⚠          | UART1 RX ← TTLinker TXD (USB D+); ver nota servo        |
| 20    | EM USO ⚠          | UART1 TX → TTLinker RXD (USB D-); ver nota servo        |
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

## Periféricos da Freenove

### Display — ST7789 2" (SPI2)

| Sinal | GPIO | Notas                                              |
| ----- | ---- | -------------------------------------------------- |
| SCLK  | 47   | SPI2 clock                                         |
| MOSI  | 21   | SPI2 data                                          |
| MISO  | —    | Não usado pelo ST7789 (sem leitura)                |
| CS    | GND  | Tied GND — display sempre selecionado              |
| DC    | 45   | Data/Command ⚠ strapping pin (VDD_SPI); ver nota  |
| RST   | 3    | Reset físico controlado pelo head                  |
| BL    | —    | Sem backlight no módulo ST7789 atual               |

Frequência SPI do gate animado DM2.2: 40 MHz com jumpers. O perfil deve voltar
a 20 MHz se houver corrupção, escurecimento, travamento ou erro durante o soak.
Orientação final validada: rotação LovyanGFX `0`, 240×320 vertical.

O RST do ST7789 deve permanecer em GPIO3, não em 3,3 V. Com RST fixo em nível
alto, o controlador do painel pode permanecer travado após reset ou reflash da
Freenove; o firmware inicia o painel corretamente somente quando pode aplicar
o pulso de reset físico.

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

### WS2812 LEDs (RMT) — legado a remover do head

| Sinal | GPIO | Notas                        |
| ----- | ---- | ---------------------------- |
| DATA  | 3    | RMT canal 0, 2 LEDs em série |

Alimentação: 5V direto. Corrente máxima: ~120mA a 100% RGB (não usar 100% em operação normal).

> **GPIO 3** sacrifica Touch T3 e JTAG e agora pertence ao reset físico do
> ST7789. Câmera DVP não usa GPIO 3 — seguro para uso permanente. O touchscreen
> futuro deverá usar outro GPIO para INT.
>
> **Por que não GPIO 19:** GPIO 19 = USB D+ do ESP32-S3. O stack WiFi usa
> `CONFIG_SOC_WIFI_PHY_NEEDS_USB_WORKAROUND` que reconfigura o bloco USB PHY ao
> conectar a um AP — isso contesta GPIO 19 com o RMT e trava as atualizações dos
> LEDs. GPIO 3 não tem essa restrição. GPIO 19 foi realocado para UART1 RX (servo),
> onde o risco de contenção com WiFi PHY é menor (UART tem maior tolerância a glitches).

### INMP441 + MAX98357A — legado a migrar para main

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

### SCS0009 — legado a migrar para main

| Lado ESP32 | GPIO | Pino TTLinker | Notas                             |
| ---------- | ---- | ------------- | --------------------------------- |
| UART1 TX   | 20   | **RXD**       | ESP32 envia → TTLinker recebe     |
| UART1 RX   | 19   | **TXD**       | ESP32 recebe ← TTLinker transmite |

> ⚠ **Labeling do TTLinker é pela perspectiva do TTLinker:**
> TXD = TTLinker transmite (→ MCU RX); RXD = TTLinker recebe (← MCU TX).
> Conectar invertido (TX→TXD, RX→RXD) causa eco sem resposta válida — o servo
> ecoa os bytes recebidos mas a resposta nunca chega ao ESP32.
> **Validado em bancada após diagnóstico extenso (maio 2026).**

O FE-TTLinker Mini V2 converte full-duplex UART (TX/RX separados, lado MCU)
para half-duplex SCS bus (DATA único, lado servo). Direção automática —
nenhum pino de controle adicional necessário.

**Eco do barramento:** o servo ecoa os bytes de TX recebidos enquanto processa
o comando. O firmware descarta esses bytes (ERR ≠ 0x00) e aguarda a resposta
válida na mesma janela de leitura.

> **Limitação conhecida — GPIO 19/20 = USB D+/D-.**
> `servo_hal_init()` chama `usb_serial_jtag_ll_phy_enable_pad(false)` e o
> WiFi usa `WIFI_PS_NONE` para minimizar interferência. TX (GPIO 20) funciona
> corretamente. RX (GPIO 19) pode receber 0 bytes durante bursts de WiFi RF
> (`CONFIG_SOC_WIFI_PHY_NEEDS_USB_WORKAROUND`); motion_safety trata como
> best-effort (loga warning, não falha). Writes/movimentos não são afetados.
>
> **Alternativas testadas e descartadas:**
> GPIO 8/9 (DVP D2/D1): OV2640 drive push-pull sem XCLK — brigariam com UART.
> GPIO 4/5 (SIOD/SIOC): OV2640 em estado indeterminado sem XCLK — sem resposta.
> GPIO 43/44: conflita com CP2102/UART0 console.
> **Fix permanente requer PCB customizado** com GPIOs dedicados para servo UART.

Baud rate: 1Mbps (padrão Feetech SCS0009).
IDs de servo: NECK\_PAN = 1, NECK\_TILT = 2.
Alimentação servos: 5V direto (linha separada, capacitor bulk obrigatório).
Alimentação TTLinker: 5V (mesmo barramento dos servos).

### Touch corporal — legado a migrar para main

| Sinal    | GPIO | Notas                              |
| -------- | ---- | ---------------------------------- |
| TOUCH\_IN | 2   | Touch T2 — fita de cobre          |

Threshold calibrado em runtime (`baseline × (1 + touch_sens*0.2/100)`).
`touch_sens` default: 25, equivalente a 5% acima do baseline para a fita de cobre
com fio atual. Valores menores, como o legado 10/2%, podem disparar por toque no
fio ou aproximação.

GPIOs touch disponíveis para expansão futura: GPIO 1 (T1, em uso como WS), GPIO 3 (T3, spare).

### Touch multi-zona futuro

Para aproximar a interação de cabeça do modelo StackChan, a evolução planejada
é adicionar um controlador capacitivo dedicado no I2C (`NB_I2C_PIN_SDA` GPIO 4,
`NB_I2C_PIN_SCL` GPIO 5) com três eletrodos de fita de cobre:

| Zona | Uso esperado |
| --- | --- |
| Esquerda | Início/fim de carinho lateral e swipe |
| Centro | Toque/cuidado central |
| Direita | Início/fim de carinho lateral e swipe |

Candidatos de hardware:

- `MPR121`: 12 canais I2C, comum em módulos prontos; usar inicialmente só 3
  entradas.
- `CAP1203`: 3 canais I2C, encaixe ideal se houver módulo pronto disponível.

O touch nativo do ESP32-S3 em GPIO 2 permanece como entrada provisória até a
placa chegar. O controlador dedicado deve ficar fisicamente perto das fitas, com
fios curtos entre módulo e eletrodos; o trecho mais longo fica no barramento I2C.

---

## Instrumentação de Energia — ADC 5 V (F49)

Para medir a tensão do barramento de 5 V (servos + LEDs) e servir à lógica de
brownout em software (F04) e ao orçamento de energia documentado em `ENERGY.md`:

### Especificação do divisor

O ESP32-S3 ADC aceita no máximo **3.1 V** na entrada (com atenuação de 12 dB).
Para medir até 6 V com margem:

```
Divisor resistivo: R1 = 68 kΩ (alta), R2 = 56 kΩ (baixa)
V_adc = V_in × R2 / (R1 + R2) = V_in × 56/124 ≈ V_in × 0.452

@ 5.0 V: V_adc ≈ 2.26 V  (dentro dos 3.1 V)
@ 5.5 V: V_adc ≈ 2.49 V  (dentro dos 3.1 V)
Tolerância dos resistores: 1% recomendado para ±50 mV de erro máximo.
```

Conectar ponto médio do divisor ao GPIO de ADC escolhido (abaixo).

### Pino sugerido

| GPIO | ADC   | Disponibilidade                                          |
|------|-------|----------------------------------------------------------|
| 35   | ADC1_CH4 | Inacessível no header (PSRAM octal) — **não usar** |
| 36   | ADC1_CH5 | Inacessível — PSRAM octal                          |
| —    | —     | **Não há GPIO ADC livre confirmado no header atual.**    |

> ⚠ **Pendente de verificação física:** o header da Freenove N16R8 expõe
> apenas os GPIOs listados no mapa de pinos. GPIOs 35–37 são internamente
> conectados ao PSRAM. Antes de soldar o divisor, confirmar com multímetro
> que o pino escolhido não mostra tensão PSRAM.
>
> **Alternativa:** medir 3.3 V regulado em vez de 5 V (mais simples, mas não
> detecta brownout do barramento de servos antes do regulador). GPIO 2 (Touch T2)
> pode ser compartilhado com ADC1_CH1 em modo single-shot durante idle, se o
> touch não estiver ativo (mutuamente exclusivo por software).

### Calibração

`esp_adc_cal_characterize()` com `ESP_ADC_CAL_VAL_EFUSE_TP` para compensar
offset de fábrica. Leitura em `adc1_get_raw()`, converter via coeficientes
e multiplicar por `(R1+R2)/R2` para obter tensão real.

Validação: leitura bate com multímetro ±5% (critério F49).

---

## Hardware Adiado (Pinos Reservados)

### OV2640 — câmera fixa do head, ativação em F4

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

Ao ativar a câmera: framebuffer somente na PSRAM do head. Manter no mínimo
300KB livres além dos buffers ativos; o requisito não se aplica à PSRAM da
main após F4.

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
