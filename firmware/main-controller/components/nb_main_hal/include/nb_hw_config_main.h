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

#define NB_MAIN_PIN_TOUCH_BODY       2

#define NB_MAIN_I2C_PORT             I2C_NUM_0
#define NB_MAIN_PIN_I2C_SDA          4
#define NB_MAIN_PIN_I2C_SCL          5
#define NB_MAIN_I2C_FREQ_HZ          400000

#define NB_MAIN_PIN_POWER_5V_ADC     7

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
_Static_assert(NB_MAIN_PIN_SERVO_TX != NB_MAIN_PIN_SERVO_RX,
               "main servo UART conflict");
_Static_assert(NB_MAIN_PIN_LED_DATA != NB_MAIN_PIN_STATUS_RGB,
               "robot LEDs/onboard status LED conflict");

#endif /* NB_HW_CONFIG_MAIN_H */
