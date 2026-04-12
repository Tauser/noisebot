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

/* ── Display ST7789 (SPI2) ───────────────────────────────────────────────── */
/* TODO(etapa-1.1): preencher após consulta ao schematic */
#define NB_DISP_PIN_SCLK    (-1)    /* SPI2 clock */
#define NB_DISP_PIN_MOSI    (-1)    /* SPI2 data  */
#define NB_DISP_PIN_CS      (-1)    /* Chip select */
#define NB_DISP_PIN_DC      (-1)    /* Data/Command */
#define NB_DISP_PIN_RST     (-1)    /* Reset (ativo baixo) */
#define NB_DISP_PIN_BL      (-1)    /* Backlight PWM (LEDC) */
#define NB_DISP_SPI_HOST    SPI2_HOST
#define NB_DISP_SPI_FREQ_KHZ 40000  /* 40MHz no bring-up; testar 80MHz */
#define NB_DISP_WIDTH       240
#define NB_DISP_HEIGHT      240

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
