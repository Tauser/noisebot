/*
 * nb_hw_config.h — Mapa de hardware do NoiseBot
 *
 * Fonte de verdade única para todos os GPIOs, hosts SPI/I2S/UART e parâmetros
 * elétricos fixos do hardware. Nenhum outro arquivo deve hardcodar números de
 * pino ou constantes de barramento.
 *
 * ATENÇÃO: Verificar cada valor contra o schematic da placa Freenove
 * ESP32-S3-WROOM CAM N16R8 antes de alimentar qualquer periférico.
 *
 * Periféricos adiados (câmera, IMU, bateria): pinos listados como reservados.
 * NUNCA realocar GPIOs marcados como RESERVADO_CAMERA.
 */

#ifndef NB_HW_CONFIG_H
#define NB_HW_CONFIG_H

/* ── microSD (SDMMC 1-bit) ───────────────────────────────────────────────── */
/*
 * SD onboard da placa Freenove ESP32-S3-WROOM CAM — interface SDMMC (não SPI).
 * 1-bit mode: CLK, CMD, DATA0. Sem CS.
 */
#define NB_SD_PIN_CLK       39
#define NB_SD_PIN_CMD       38
#define NB_SD_PIN_DATA0     40
#define NB_SD_BUS_WIDTH     1       /* 1-bit SDMMC */
#define NB_SD_MOUNT_POINT   "/sdcard"
#define NB_SD_MAX_FILES     5

/*
 * ── Display (SPI2) ─────────────────────────────────────────────────────────
 *
 * Barramente SPI2 compartilhado pelos dois painéis suportados.
 * Selecionar driver ativo com NB_DISP_DRIVER_ILI9342 (default: ST7789).
 *
 * Sinais comuns SPI2:
 */
#define NB_DISP_PIN_SCLK        47
#define NB_DISP_PIN_MOSI        21
#define NB_DISP_PIN_MISO        (-1)    /* ST7789 não usa MISO              */
#define NB_DISP_PIN_DC          45      /* Data/Command                      */
#define NB_DISP_SPI_HOST        SPI2_HOST
#define NB_DISP_SPI_FREQ_KHZ    50000   /* 50MHz — 60 e 80MHz falharam (sem display) */

/* ── ST7789 240×240 (painel atual) ──────────────────────────────────────── */
#define NB_DISP_ST7789_PIN_CS   (-1)    /* CS ligado ao GND — sempre ativo  */
#define NB_DISP_ST7789_PIN_RST  (-1)    /* Sem pino RST — software reset    */
#define NB_DISP_ST7789_PIN_BL   (-1)    /* Sem backlight no módulo atual     */
#define NB_DISP_ST7789_WIDTH    320     /* landscape                         */
#define NB_DISP_ST7789_HEIGHT   240

/* ── ILI9342 320×240 (painel futuro) ────────────────────────────────────── */
/* TODO(etapa-1.1-ili9342): preencher quando painel for conectado */
#define NB_DISP_ILI9342_PIN_CS  (-1)    /* TBD                              */
#define NB_DISP_ILI9342_PIN_RST (-1)    /* TBD                              */
#define NB_DISP_ILI9342_PIN_BL  (-1)    /* TBD — backlight via LEDC         */
#define NB_DISP_ILI9342_WIDTH   320
#define NB_DISP_ILI9342_HEIGHT  240

/* Backlight LEDC (usado pelo ILI9342) */
#define NB_DISP_LEDC_CHANNEL    0
#define NB_DISP_LEDC_TIMER      0

/* Largura/altura do driver ativo (usado pelo render_service) */
#if defined(NB_DISP_DRIVER_ILI9342)
#  define NB_DISP_WIDTH   NB_DISP_ILI9342_WIDTH
#  define NB_DISP_HEIGHT  NB_DISP_ILI9342_HEIGHT
#else
#  define NB_DISP_WIDTH   NB_DISP_ST7789_WIDTH
#  define NB_DISP_HEIGHT  NB_DISP_ST7789_HEIGHT
#endif

/* ── WS2812 LEDs (RMT) ───────────────────────────────────────────────────── */
#define NB_LED_PIN_DATA     3       /* RMT canal 0 — 2 LEDs em série (GPIO19=USB-D+, disputado pelo USB PHY) */
#define NB_LED_COUNT        2
#define NB_LED_RMT_CHANNEL  0 

/* ── Áudio Full-Duplex (I2S0 — mic RX + speaker TX compartilhados) ──────── */
/*
 * INMP441 (RX) e MAX98357A (TX) compartilham I2S0 em modo full-duplex.
 * BCLK e WS são comuns (GPIO 41/42); dados em pinos separados.
 * Restrição: ambos operam no mesmo sample rate (16kHz).
 *
 * GPIO 41/42 escolhidos para clock pois não têm função touch nem strapping.
 * GPIO 14 (MIC SD) e GPIO 1 (SPK DIN) sacrificam TOUCH_PAD_NUM14 e T1.
 */
#define NB_AUDIO_I2S_PORT    I2S_NUM_0
#define NB_AUDIO_SAMPLE_RATE 16000
#define NB_AUDIO_PIN_BCLK    41     /* I2S BCLK — compartilhado mic + speaker */
#define NB_AUDIO_PIN_WS      42     /* I2S LRCK — compartilhado mic + speaker */

/* ── INMP441 Microfone (I2S0 RX) ─────────────────────────────────────────── */
#define NB_MIC_PIN_WS       NB_AUDIO_PIN_WS   /* GPIO 42 — compartilhado */
#define NB_MIC_PIN_SCK      NB_AUDIO_PIN_BCLK /* GPIO 41 — compartilhado */
#define NB_MIC_PIN_SD       14      /* Serial data (RX) — sacrifica TOUCH_PAD_NUM14 */
#define NB_MIC_I2S_PORT     NB_AUDIO_I2S_PORT
#define NB_MIC_SAMPLE_RATE  NB_AUDIO_SAMPLE_RATE
#define NB_MIC_BITS         32      /* 32 bits por sample; 24 úteis */

/* ── MAX98357A Speaker (I2S0 TX, full-duplex com mic) ────────────────────── */
/*
 * SD_MODE: tied HIGH externamente (3.3V via resistor 10kΩ).
 * Amplificador sempre ativo; controle de volume via divisão digital do PCM.
 * GPIO 48 (LED onboard da placa) não é usado aqui — reservado como LED de status.
 */
#define NB_SPK_PIN_BCLK     NB_AUDIO_PIN_BCLK /* GPIO 41 — compartilhado */
#define NB_SPK_PIN_LRC      NB_AUDIO_PIN_WS   /* GPIO 42 — compartilhado */
#define NB_SPK_PIN_DIN      1       /* Serial data (TX) — sacrifica TOUCH_PAD_NUM1 */
#define NB_SPK_PIN_SD_MODE  (-1)    /* Tied HIGH externamente — sem controle SW */
#define NB_SPK_I2S_PORT     NB_AUDIO_I2S_PORT

/* ── SCS0009 Servos (UART1 via FE-TTLinker) ──────────────────────────────── */
/*
 * GPIO 20 (TX) e GPIO 19 (RX): únicos GPIOs livres no header da placa.
 * GPIO 33 não existe no header da Freenove ESP32-S3-WROOM CAM — descartado.
 * GPIO 46 descartado para RX: pull-down interno habilita debug UART0 em download
 * mode, corrompendo uploads.
 * GPIO 19 = USB D+ — risco de contenção com WiFi PHY na etapa 9.6. Reavaliar
 * quando WiFi for ativado (CONFIG_SOC_WIFI_PHY_NEEDS_USB_WORKAROUND).
 */
#define NB_SERVO_PIN_TX     20      /* UART1 TX → FE-TTLinker RX */
#define NB_SERVO_PIN_RX     19      /* UART1 RX ← FE-TTLinker TX (único GPIO livre no header) */
#define NB_SERVO_UART_PORT  UART_NUM_1
#define NB_SERVO_BAUD_RATE  1000000 /* 1Mbps padrão Feetech */
#define NB_SERVO_ID_PAN     1
#define NB_SERVO_ID_TILT    2

/* ── I2C Master (GPIO 4/5) — barramento compartilhado de sensores ────────── */
/*
 * Inicializado pelo nb_i2c_hal. Todos os HALs de sensor usam este barramento.
 * Sensores com pino INT (IMU, APDS-9960) usam polling — não há GPIO livre
 * para interrupção (GPIO 3 está ocupado pelo WS2812 RMT).
 *
 * Endereços no barramento:
 *   0x3C  OV2640 câmera (SCCB)      — etapa 8.1
 *   0x68  MPU-6050 IMU              — etapa 8.2
 *   0x44  SHT40 temp/humidade       — etapa 8.3
 *   0x39  APDS-9960 proximidade     — etapa 8.4
 *   0x6B  bq25185 charger           — etapa 8.5
 *   0x36  MAX17048 fuel gauge        — etapa 8.5
 *   0x5A  MPR121 touch zones        — etapa 8.6
 */
#define NB_I2C_PORT         I2C_NUM_0
#define NB_I2C_PIN_SDA      4
#define NB_I2C_PIN_SCL      5
#define NB_I2C_FREQ_HZ      400000      /* Fast Mode 400kHz */

/* ── Touch — fita de cobre ───────────────────────────────────────────────── */
#define NB_TOUCH_PIN        2       /* GPIO com função touch ESP32-S3        */
#define NB_TOUCH_PAD_NUM    2       /* TOUCH_PAD_NUM2 (GPIO2 no ESP32-S3)    */

/* ── Câmera OV2640 (DVP) — ADIADA ────────────────────────────────────────── */
/*
 * RESERVADO_CAMERA: pinos fisicamente soldados na placa Freenove N16R8.
 * NUNCA realocar estes GPIOs — estão no header E no conector FPC da câmera.
 *
 * Confirmado pelo pinout oficial da placa (docs/ESP32S3_Pinout.png):
 *   D0 (Y2)=11  D1 (Y3)=9   D2 (Y4)=8   D3 (Y5)=10
 *   D4 (Y6)=12  D5 (Y7)=18  D6 (Y8)=17  D7 (Y9)=16
 *   PCLK=13     XCLK=15     VSYNC=6     HREF=7
 *   SIOD (SDA)=4             SIOC (SCL)=5
 *   RESET=-1 (tied 3V3)     PWDN=-1 (tied GND)
 *
 * I2C câmera (GPIO 4/5) pode ser compartilhado com IMU e fuel gauge
 * quando etapa-8.x for implementada (endereços não colidem).
 *
 * TODO(etapa-8.1): inicializar esp32-camera com os pinos acima.
 */
#define NB_CAM_PIN_D0       11
#define NB_CAM_PIN_D1       9
#define NB_CAM_PIN_D2       8
#define NB_CAM_PIN_D3       10
#define NB_CAM_PIN_D4       12
#define NB_CAM_PIN_D5       18
#define NB_CAM_PIN_D6       17
#define NB_CAM_PIN_D7       16
#define NB_CAM_PIN_PCLK     13
#define NB_CAM_PIN_XCLK     15
#define NB_CAM_PIN_VSYNC    6
#define NB_CAM_PIN_HREF     7
#define NB_CAM_PIN_SIOD     4       /* I2C SDA — compartilhado com IMU */
#define NB_CAM_PIN_SIOC     5       /* I2C SCL — compartilhado com IMU */
#define NB_CAM_PIN_RESET    (-1)    /* Tied 3.3V na placa */
#define NB_CAM_PIN_PWDN     (-1)    /* Tied GND na placa */

/* ── Bridge LLM (Etapa 12.1) ─────────────────────────────────────────────── */
#define NB_BRIDGE_TCP_PORT      9000    /* ESP32 serve como TCP server       */
/* UART fallback: USB CDC nativo do ESP32-S3 (usb_serial_jtag driver).
 * Requer CONFIG_ESP_CONSOLE_UART_DEFAULT=UART0 em menuconfig para separar
 * o console de debug da interface bridge (porta USB nativa). */

/* ── GPIOs Livres / Reserva ───────────────────────────────────────────────── */
/*
 * Não há GPIOs livres no header da placa Freenove ESP32-S3-WROOM CAM N16R8.
 * GPIO 19 (UART1 RX servo) é o último disponível — nenhuma reserva restante.
 *
 * GPIO 48:      LED onboard azul da placa Freenove — reservado para status visual
 *               NÃO repurposear: útil em debug e diferencia estados de boot
 *
 * GPIOs que NUNCA devem ser usados:
 *   GPIO 0        — strapping boot mode (pull-down = download mode)
 *   GPIO 33       — NÃO existe no header da placa (ausente no conector físico)
 *   GPIO 43/44    — UART0 TX/RX (debug/programming)
 *   GPIO 26–37    — PSRAM/Flash interno (inacessíveis no N16R8; 35–37 no header mas em uso pelo PSRAM)
 *
 * GPIO 45 (display DC): strapping VDD_SPI — já comprometido, funcional.
 *   Verificar com osciloscópio se há glitch LOW durante power-on.
 *   Se houver: adicionar pull-up 10kΩ externo.
 *
 * GPIO 46: NÃO usar para UART RX nem para qualquer sinal TTL idle-HIGH.
 *   Pull-down interno em reset faz ROM habilitar debug na UART0 em download
 *   mode quando GPIO 46 está puxado HIGH — corrompe uploads via esptool.
 */

#endif /* NB_HW_CONFIG_H */
