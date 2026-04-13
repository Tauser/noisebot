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
#define NB_DISP_SPI_FREQ_KHZ    60000   /* 60MHz — testar; 80MHz falhou     */

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
/* TODO(etapa-2.1): preencher após consulta ao schematic */
#define NB_LED_PIN_DATA     (-1)    /* RMT canal 0 */
#define NB_LED_COUNT        2
#define NB_LED_RMT_CHANNEL  0

/* ── INMP441 Microfone (I2S0 RX) ─────────────────────────────────────────── */
/* TODO(etapa-4.1): preencher após consulta ao schematic */
#define NB_MIC_PIN_WS       (-1)    /* Word select (LRCK) */
#define NB_MIC_PIN_SCK      (-1)    /* Bit clock */
#define NB_MIC_PIN_SD       (-1)    /* Serial data */
#define NB_MIC_I2S_PORT     I2S_NUM_0
#define NB_MIC_SAMPLE_RATE  16000
#define NB_MIC_BITS         32      /* 32 bits por sample; 24 úteis */

/* ── MAX98357A Speaker (I2S1 TX) ─────────────────────────────────────────── */
/* TODO(etapa-4.2): preencher após consulta ao schematic */
#define NB_SPK_PIN_BCLK     (-1)
#define NB_SPK_PIN_LRC      (-1)
#define NB_SPK_PIN_DIN      (-1)
#define NB_SPK_PIN_SD_MODE  (-1)    /* HIGH=ativo, LOW=shutdown */
#define NB_SPK_I2S_PORT     I2S_NUM_1

/* ── SCS0009 Servos (UART1 via FE-TTLinker) ──────────────────────────────── */
/* TODO(etapa-3.1): preencher após consulta ao schematic */
#define NB_SERVO_PIN_TX     (-1)    /* UART1 TX → FE-TTLinker RX */
#define NB_SERVO_PIN_RX     (-1)    /* UART1 RX ← FE-TTLinker TX */
#define NB_SERVO_UART_PORT  UART_NUM_1
#define NB_SERVO_BAUD_RATE  1000000 /* 1Mbps padrão Feetech */
#define NB_SERVO_ID_PAN     1
#define NB_SERVO_ID_TILT    2

/* ── Touch — fita de cobre ───────────────────────────────────────────────── */
/* TODO(etapa-2.2): preencher após consulta ao schematic */
#define NB_TOUCH_PIN        (-1)    /* GPIO com função touch ESP32-S3 */

/* ── Câmera OV2640 (DVP) — ADIADA ────────────────────────────────────────── */
/*
 * RESERVADO_CAMERA: pinos fisicamente conectados na placa.
 * NUNCA realocar estes GPIOs para outro uso.
 * TODO(etapa-8.1): mapear D0–D7, XCLK, PCLK, VSYNC, HREF, SIOD, SIOC, RST, PWDN
 */

#endif /* NB_HW_CONFIG_H */
