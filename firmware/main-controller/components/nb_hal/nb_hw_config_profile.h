/*
 * nb_hw_config_profile.h — seleção de mapa de hardware do body/main
 *
 * Este header expõe o conjunto de macros usado pelos HALs que pertencem à
 * Waveshare main-controller (áudio, LEDs, touch, I2C, servo). O mapa legacy
 * continua em nb_hw_config.h.
 *
 * A seleção é explícita via Kconfig:
 *   - legacy: usa o mapa atual da Freenove;
 *   - Waveshare: mapeia os sinais para nb_hw_config_main.h.
 */

#ifndef NB_HW_CONFIG_PROFILE_H
#define NB_HW_CONFIG_PROFILE_H

#include "sdkconfig.h"

#if defined(CONFIG_NB_BOARD_PROFILE_WAVESHARE) && CONFIG_NB_BOARD_PROFILE_WAVESHARE
#include "nb_hw_config_main.h"

#define NB_BOARD_PROFILE_NAME "NoiseBot ESP32-S3 N32R16V"

#define NB_AUDIO_I2S_PORT    NB_MAIN_AUDIO_I2S_PORT
#define NB_AUDIO_SAMPLE_RATE NB_MAIN_AUDIO_SAMPLE_RATE
#define NB_AUDIO_PIN_BCLK    NB_MAIN_PIN_AUDIO_BCLK
#define NB_AUDIO_PIN_WS      NB_MAIN_PIN_AUDIO_WS

#define NB_MIC_PIN_WS        NB_AUDIO_PIN_WS
#define NB_MIC_PIN_SCK       NB_AUDIO_PIN_BCLK
#define NB_MIC_PIN_SD        NB_MAIN_PIN_MIC_SD
#define NB_MIC_I2S_PORT      NB_AUDIO_I2S_PORT
#define NB_MIC_SAMPLE_RATE   NB_AUDIO_SAMPLE_RATE
#define NB_MIC_BITS          32

#define NB_SPK_PIN_BCLK      NB_AUDIO_PIN_BCLK
#define NB_SPK_PIN_LRC       NB_AUDIO_PIN_WS
#define NB_SPK_PIN_DIN       NB_MAIN_PIN_SPK_DIN
#define NB_SPK_PIN_SD_MODE   (-1)
#define NB_SPK_I2S_PORT      NB_AUDIO_I2S_PORT

#define NB_SERVO_PIN_TX      NB_MAIN_PIN_SERVO_TX
#define NB_SERVO_PIN_RX      NB_MAIN_PIN_SERVO_RX
#define NB_SERVO_UART_PORT   NB_MAIN_SERVO_UART_PORT
#define NB_SERVO_BAUD_RATE   NB_MAIN_SERVO_BAUD_RATE
#define NB_SERVO_ID_PAN      NB_MAIN_SERVO_ID_PAN
#define NB_SERVO_ID_TILT     NB_MAIN_SERVO_ID_TILT

#define NB_TOUCH_PIN         NB_MAIN_PIN_TOUCH_BODY
#define NB_TOUCH_PAD_NUM     NB_MAIN_PIN_TOUCH_BODY

#define NB_I2C_PORT          NB_MAIN_I2C_PORT
#define NB_I2C_PIN_SDA       NB_MAIN_PIN_I2C_SDA
#define NB_I2C_PIN_SCL       NB_MAIN_PIN_I2C_SCL
#define NB_I2C_FREQ_HZ       NB_MAIN_I2C_FREQ_HZ

#define NB_LED_PIN_DATA      NB_MAIN_PIN_LED_DATA
#define NB_LED_COUNT         NB_MAIN_LED_COUNT
#define NB_LED_RMT_CHANNEL   0

#define NB_POWER_PIN_5V_ADC  NB_MAIN_PIN_POWER_5V_ADC
#define NB_POWER_5V_ADC_UNIT NB_MAIN_POWER_5V_ADC_UNIT
#define NB_POWER_5V_ADC_CHANNEL NB_MAIN_POWER_5V_ADC_CHANNEL
#define NB_POWER_5V_DIVIDER_R1_OHM NB_MAIN_POWER_5V_DIVIDER_R1_OHM
#define NB_POWER_5V_DIVIDER_R2_OHM NB_MAIN_POWER_5V_DIVIDER_R2_OHM

#else
#include "nb_hw_config.h"

#define NB_BOARD_PROFILE_NAME "NoiseBot ESP32-S3 N16R8"
#endif

#endif /* NB_HW_CONFIG_PROFILE_H */
