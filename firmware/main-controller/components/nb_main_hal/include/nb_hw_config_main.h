/*
 * nb_hw_config_main.h — estado-alvo da Waveshare ESP32-S3 N32R16V.
 *
 * PROPOSTA PENDENTE DE BANCADA. Não selecionar este mapa no firmware de
 * produção antes de confirmar a variante/silk da placa e o gate elétrico de
 * docs/GPIO_DUAL_MCU.md.
 */

#ifndef NB_HW_CONFIG_MAIN_H
#define NB_HW_CONFIG_MAIN_H

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"

#define NB_MAIN_PIN_TOUCH_BODY       2

#define NB_MAIN_I2C_PORT             I2C_NUM_0
#define NB_MAIN_PIN_I2C_SDA          4
#define NB_MAIN_PIN_I2C_SCL          5
#define NB_MAIN_I2C_FREQ_HZ          400000

#define NB_MAIN_PIN_POWER_5V_ADC     7
#define NB_MAIN_POWER_5V_ADC_UNIT    ADC_UNIT_1
#define NB_MAIN_POWER_5V_ADC_CHANNEL ADC_CHANNEL_6
/*
 * DMM.3 de bancada (2026-06-21): divisor externo temporário 100k/100k
 * montado na protoboard. O monitor de 5V exige divisor externo; a topologia
 * física final ainda pode migrar para solução mais permanente.
 */
#define NB_MAIN_POWER_5V_DIVIDER_R1_OHM 100000U
#define NB_MAIN_POWER_5V_DIVIDER_R2_OHM 100000U

#define NB_MAIN_PIN_HEAD_RESET       8
#define NB_MAIN_LINK_SPI_HOST        SPI2_HOST
#define NB_MAIN_PIN_LINK_CS          10
#define NB_MAIN_PIN_LINK_MOSI        11
#define NB_MAIN_PIN_LINK_SCLK        12
#define NB_MAIN_PIN_LINK_MISO        13
#define NB_MAIN_PIN_HEAD_IRQ         14
#define NB_MAIN_LINK_BRINGUP_HZ      10000000
#define NB_MAIN_LINK_VALIDATED_HZ    20000000

#define NB_MAIN_SERVO_UART_PORT      UART_NUM_1
#define NB_MAIN_PIN_SERVO_TX         17
#define NB_MAIN_PIN_SERVO_RX         18
#define NB_MAIN_SERVO_BAUD_RATE      1000000
#define NB_MAIN_SERVO_ID_PAN         1
#define NB_MAIN_SERVO_ID_TILT        2

#define NB_MAIN_PIN_USB_DN           19
#define NB_MAIN_PIN_USB_DP           20

#define NB_MAIN_PIN_LED_DATA         21
#define NB_MAIN_LED_COUNT            2
#define NB_MAIN_PIN_STATUS_RGB       38

#define NB_MAIN_AUDIO_I2S_PORT       I2S_NUM_0
#define NB_MAIN_AUDIO_SAMPLE_RATE    16000
#define NB_MAIN_PIN_MIC_SD           39
#define NB_MAIN_PIN_AUDIO_BCLK       40
#define NB_MAIN_PIN_AUDIO_WS         41
#define NB_MAIN_PIN_SPK_DIN          42

#define NB_MAIN_PIN_UART0_TX         43
#define NB_MAIN_PIN_UART0_RX         44

/* Header presente, mas sem conexão funcional no módulo octal. */
#define NB_MAIN_PIN_NC_35            35
#define NB_MAIN_PIN_NC_36            36
#define NB_MAIN_PIN_NC_37            37

/* Domínio VDD_SPI de 1,8V na variante N32R16V: não usar em lógica 3,3V. */
#define NB_MAIN_PIN_VDD_SPI_1V8_47   47
#define NB_MAIN_PIN_VDD_SPI_1V8_48   48

_Static_assert(NB_MAIN_PIN_LINK_CS != NB_MAIN_PIN_HEAD_IRQ,
               "main link CS/IRQ conflict");
_Static_assert(NB_MAIN_PIN_LINK_CS != NB_MAIN_PIN_LINK_MOSI,
               "main link CS/MOSI conflict");
_Static_assert(NB_MAIN_PIN_LINK_CS != NB_MAIN_PIN_LINK_SCLK,
               "main link CS/SCLK conflict");
_Static_assert(NB_MAIN_PIN_LINK_CS != NB_MAIN_PIN_LINK_MISO,
               "main link CS/MISO conflict");
_Static_assert(NB_MAIN_PIN_LINK_MOSI != NB_MAIN_PIN_LINK_SCLK,
               "main link MOSI/SCLK conflict");
_Static_assert(NB_MAIN_PIN_LINK_MOSI != NB_MAIN_PIN_LINK_MISO,
               "main link MOSI/MISO conflict");
_Static_assert(NB_MAIN_PIN_LINK_SCLK != NB_MAIN_PIN_LINK_MISO,
               "main link SCLK/MISO conflict");
_Static_assert(NB_MAIN_PIN_HEAD_IRQ != NB_MAIN_PIN_LINK_MOSI,
               "main head IRQ/MOSI conflict");
_Static_assert(NB_MAIN_PIN_HEAD_IRQ != NB_MAIN_PIN_LINK_SCLK,
               "main head IRQ/SCLK conflict");
_Static_assert(NB_MAIN_PIN_HEAD_IRQ != NB_MAIN_PIN_LINK_MISO,
               "main head IRQ/MISO conflict");
_Static_assert(NB_MAIN_PIN_SERVO_TX != NB_MAIN_PIN_SERVO_RX,
               "main servo UART conflict");
_Static_assert(NB_MAIN_SERVO_ID_PAN != NB_MAIN_SERVO_ID_TILT,
               "main servo IDs conflict");
_Static_assert(NB_MAIN_PIN_LED_DATA != NB_MAIN_PIN_STATUS_RGB,
               "robot LEDs/onboard status LED conflict");
_Static_assert(NB_MAIN_PIN_AUDIO_BCLK != NB_MAIN_PIN_AUDIO_WS,
               "main audio BCLK/WS conflict");
_Static_assert(NB_MAIN_PIN_AUDIO_BCLK != NB_MAIN_PIN_MIC_SD,
               "main audio BCLK/MIC conflict");
_Static_assert(NB_MAIN_PIN_AUDIO_BCLK != NB_MAIN_PIN_SPK_DIN,
               "main audio BCLK/SPK conflict");
_Static_assert(NB_MAIN_PIN_AUDIO_WS != NB_MAIN_PIN_MIC_SD,
               "main audio WS/MIC conflict");
_Static_assert(NB_MAIN_PIN_AUDIO_WS != NB_MAIN_PIN_SPK_DIN,
               "main audio WS/SPK conflict");
_Static_assert(NB_MAIN_PIN_MIC_SD != NB_MAIN_PIN_SPK_DIN,
               "main audio MIC/SPK conflict");
_Static_assert(NB_MAIN_PIN_TOUCH_BODY != NB_MAIN_PIN_LINK_CS,
               "main touch/link CS conflict");
_Static_assert(NB_MAIN_PIN_TOUCH_BODY != NB_MAIN_PIN_LINK_MOSI,
               "main touch/link MOSI conflict");
_Static_assert(NB_MAIN_PIN_TOUCH_BODY != NB_MAIN_PIN_LINK_SCLK,
               "main touch/link SCLK conflict");
_Static_assert(NB_MAIN_PIN_TOUCH_BODY != NB_MAIN_PIN_LINK_MISO,
               "main touch/link MISO conflict");
_Static_assert(NB_MAIN_PIN_TOUCH_BODY != NB_MAIN_PIN_HEAD_IRQ,
               "main touch/head IRQ conflict");
_Static_assert(NB_MAIN_PIN_TOUCH_BODY != NB_MAIN_PIN_AUDIO_BCLK,
               "main touch/audio BCLK conflict");
_Static_assert(NB_MAIN_PIN_TOUCH_BODY != NB_MAIN_PIN_AUDIO_WS,
               "main touch/audio WS conflict");
_Static_assert(NB_MAIN_PIN_TOUCH_BODY != NB_MAIN_PIN_MIC_SD,
               "main touch/MIC conflict");
_Static_assert(NB_MAIN_PIN_TOUCH_BODY != NB_MAIN_PIN_SPK_DIN,
               "main touch/SPK conflict");
_Static_assert(NB_MAIN_PIN_TOUCH_BODY != NB_MAIN_PIN_SERVO_TX,
               "main touch/servo TX conflict");
_Static_assert(NB_MAIN_PIN_TOUCH_BODY != NB_MAIN_PIN_SERVO_RX,
               "main touch/servo RX conflict");
_Static_assert(NB_MAIN_PIN_TOUCH_BODY != NB_MAIN_PIN_LED_DATA,
               "main touch/robot LED conflict");
_Static_assert(NB_MAIN_PIN_TOUCH_BODY != NB_MAIN_PIN_STATUS_RGB,
               "main touch/status LED conflict");
_Static_assert(NB_MAIN_PIN_TOUCH_BODY != NB_MAIN_PIN_USB_DN,
               "main touch/USB DN conflict");
_Static_assert(NB_MAIN_PIN_TOUCH_BODY != NB_MAIN_PIN_USB_DP,
               "main touch/USB DP conflict");
_Static_assert(NB_MAIN_PIN_LINK_CS != NB_MAIN_PIN_AUDIO_BCLK,
               "main link CS/audio BCLK conflict");
_Static_assert(NB_MAIN_PIN_LINK_CS != NB_MAIN_PIN_AUDIO_WS,
               "main link CS/audio WS conflict");
_Static_assert(NB_MAIN_PIN_LINK_CS != NB_MAIN_PIN_MIC_SD,
               "main link CS/MIC conflict");
_Static_assert(NB_MAIN_PIN_LINK_CS != NB_MAIN_PIN_SPK_DIN,
               "main link CS/SPK conflict");
_Static_assert(NB_MAIN_PIN_LINK_CS != NB_MAIN_PIN_SERVO_TX,
               "main link CS/servo TX conflict");
_Static_assert(NB_MAIN_PIN_LINK_CS != NB_MAIN_PIN_SERVO_RX,
               "main link CS/servo RX conflict");
_Static_assert(NB_MAIN_PIN_LINK_CS != NB_MAIN_PIN_LED_DATA,
               "main link CS/robot LED conflict");
_Static_assert(NB_MAIN_PIN_LINK_CS != NB_MAIN_PIN_STATUS_RGB,
               "main link CS/status LED conflict");
_Static_assert(NB_MAIN_PIN_LINK_MOSI != NB_MAIN_PIN_AUDIO_BCLK,
               "main link MOSI/audio BCLK conflict");
_Static_assert(NB_MAIN_PIN_LINK_MOSI != NB_MAIN_PIN_AUDIO_WS,
               "main link MOSI/audio WS conflict");
_Static_assert(NB_MAIN_PIN_LINK_MOSI != NB_MAIN_PIN_MIC_SD,
               "main link MOSI/MIC conflict");
_Static_assert(NB_MAIN_PIN_LINK_MOSI != NB_MAIN_PIN_SPK_DIN,
               "main link MOSI/SPK conflict");
_Static_assert(NB_MAIN_PIN_LINK_MOSI != NB_MAIN_PIN_SERVO_TX,
               "main link MOSI/servo TX conflict");
_Static_assert(NB_MAIN_PIN_LINK_MOSI != NB_MAIN_PIN_SERVO_RX,
               "main link MOSI/servo RX conflict");
_Static_assert(NB_MAIN_PIN_LINK_SCLK != NB_MAIN_PIN_AUDIO_BCLK,
               "main link SCLK/audio BCLK conflict");
_Static_assert(NB_MAIN_PIN_LINK_SCLK != NB_MAIN_PIN_AUDIO_WS,
               "main link SCLK/audio WS conflict");
_Static_assert(NB_MAIN_PIN_LINK_SCLK != NB_MAIN_PIN_MIC_SD,
               "main link SCLK/MIC conflict");
_Static_assert(NB_MAIN_PIN_LINK_SCLK != NB_MAIN_PIN_SPK_DIN,
               "main link SCLK/SPK conflict");
_Static_assert(NB_MAIN_PIN_LINK_SCLK != NB_MAIN_PIN_SERVO_TX,
               "main link SCLK/servo TX conflict");
_Static_assert(NB_MAIN_PIN_LINK_SCLK != NB_MAIN_PIN_SERVO_RX,
               "main link SCLK/servo RX conflict");
_Static_assert(NB_MAIN_PIN_LINK_MISO != NB_MAIN_PIN_AUDIO_BCLK,
               "main link MISO/audio BCLK conflict");
_Static_assert(NB_MAIN_PIN_LINK_MISO != NB_MAIN_PIN_AUDIO_WS,
               "main link MISO/audio WS conflict");
_Static_assert(NB_MAIN_PIN_LINK_MISO != NB_MAIN_PIN_MIC_SD,
               "main link MISO/MIC conflict");
_Static_assert(NB_MAIN_PIN_LINK_MISO != NB_MAIN_PIN_SPK_DIN,
               "main link MISO/SPK conflict");
_Static_assert(NB_MAIN_PIN_LINK_MISO != NB_MAIN_PIN_SERVO_TX,
               "main link MISO/servo TX conflict");
_Static_assert(NB_MAIN_PIN_LINK_MISO != NB_MAIN_PIN_SERVO_RX,
               "main link MISO/servo RX conflict");
_Static_assert(NB_MAIN_PIN_SERVO_TX != NB_MAIN_PIN_USB_DN,
               "main servo TX/USB DN conflict");
_Static_assert(NB_MAIN_PIN_SERVO_TX != NB_MAIN_PIN_USB_DP,
               "main servo TX/USB DP conflict");
_Static_assert(NB_MAIN_PIN_SERVO_RX != NB_MAIN_PIN_USB_DN,
               "main servo RX/USB DN conflict");
_Static_assert(NB_MAIN_PIN_SERVO_RX != NB_MAIN_PIN_USB_DP,
               "main servo RX/USB DP conflict");
_Static_assert(NB_MAIN_PIN_USB_DN != NB_MAIN_PIN_USB_DP,
               "main USB D-/D+ conflict");
_Static_assert(NB_MAIN_PIN_LED_DATA != NB_MAIN_PIN_AUDIO_BCLK,
               "main robot LED/audio BCLK conflict");
_Static_assert(NB_MAIN_PIN_LED_DATA != NB_MAIN_PIN_AUDIO_WS,
               "main robot LED/audio WS conflict");
_Static_assert(NB_MAIN_PIN_LED_DATA != NB_MAIN_PIN_MIC_SD,
               "main robot LED/MIC conflict");
_Static_assert(NB_MAIN_PIN_LED_DATA != NB_MAIN_PIN_SPK_DIN,
               "main robot LED/SPK conflict");
_Static_assert(NB_MAIN_PIN_LED_DATA != NB_MAIN_PIN_SERVO_TX,
               "main robot LED/servo TX conflict");
_Static_assert(NB_MAIN_PIN_LED_DATA != NB_MAIN_PIN_SERVO_RX,
               "main robot LED/servo RX conflict");
_Static_assert(NB_MAIN_PIN_LED_DATA != NB_MAIN_PIN_USB_DN,
               "main robot LED/USB DN conflict");
_Static_assert(NB_MAIN_PIN_LED_DATA != NB_MAIN_PIN_USB_DP,
               "main robot LED/USB DP conflict");
_Static_assert(NB_MAIN_PIN_STATUS_RGB != NB_MAIN_PIN_AUDIO_BCLK,
               "main status LED/audio BCLK conflict");
_Static_assert(NB_MAIN_PIN_STATUS_RGB != NB_MAIN_PIN_AUDIO_WS,
               "main status LED/audio WS conflict");
_Static_assert(NB_MAIN_PIN_STATUS_RGB != NB_MAIN_PIN_MIC_SD,
               "main status LED/MIC conflict");
_Static_assert(NB_MAIN_PIN_STATUS_RGB != NB_MAIN_PIN_SPK_DIN,
               "main status LED/SPK conflict");
_Static_assert(NB_MAIN_PIN_STATUS_RGB != NB_MAIN_PIN_SERVO_TX,
               "main status LED/servo TX conflict");
_Static_assert(NB_MAIN_PIN_STATUS_RGB != NB_MAIN_PIN_SERVO_RX,
               "main status LED/servo RX conflict");
_Static_assert(NB_MAIN_PIN_STATUS_RGB != NB_MAIN_PIN_USB_DN,
               "main status LED/USB DN conflict");
_Static_assert(NB_MAIN_PIN_STATUS_RGB != NB_MAIN_PIN_USB_DP,
               "main status LED/USB DP conflict");

#endif /* NB_HW_CONFIG_MAIN_H */
