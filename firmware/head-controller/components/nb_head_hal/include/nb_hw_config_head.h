/*
 * nb_hw_config_head.h — estado-alvo da Freenove ESP32-S3 CAM N16R8.
 *
 * GPIOs de enlace marcados abaixo conflitam com o baseline monolítico.
 * Não conectar as placas antes do gate elétrico de docs/GPIO_DUAL_MCU.md.
 */

#ifndef NB_HW_CONFIG_HEAD_H
#define NB_HW_CONFIG_HEAD_H

#include "driver/i2c_master.h"
#include "driver/spi_master.h"

#define NB_HEAD_LINK_SPI_HOST        SPI3_HOST
#define NB_HEAD_PIN_LINK_CS          1
#define NB_HEAD_PIN_HEAD_IRQ         2
#define NB_HEAD_PIN_TOUCH_INT        3
#define NB_HEAD_PIN_LINK_MISO        14
#define NB_HEAD_PIN_LINK_SCLK        41
#define NB_HEAD_PIN_LINK_MOSI        42

#define NB_HEAD_I2C_PORT             I2C_NUM_0
#define NB_HEAD_PIN_I2C_SDA          4
#define NB_HEAD_PIN_I2C_SCL          5
#define NB_HEAD_I2C_FREQ_HZ          400000

#define NB_HEAD_PIN_CAM_VSYNC        6
#define NB_HEAD_PIN_CAM_HREF         7
#define NB_HEAD_PIN_CAM_D2           8
#define NB_HEAD_PIN_CAM_D1           9
#define NB_HEAD_PIN_CAM_D3           10
#define NB_HEAD_PIN_CAM_D0           11
#define NB_HEAD_PIN_CAM_D4           12
#define NB_HEAD_PIN_CAM_PCLK         13
#define NB_HEAD_PIN_CAM_XCLK         15
#define NB_HEAD_PIN_CAM_D7           16
#define NB_HEAD_PIN_CAM_D6           17
#define NB_HEAD_PIN_CAM_D5           18
#define NB_HEAD_PIN_CAM_SIOD         NB_HEAD_PIN_I2C_SDA
#define NB_HEAD_PIN_CAM_SIOC         NB_HEAD_PIN_I2C_SCL
#define NB_HEAD_PIN_CAM_RESET        (-1)
#define NB_HEAD_PIN_CAM_PWDN         (-1)

#define NB_HEAD_DISP_SPI_HOST        SPI2_HOST
#define NB_HEAD_PIN_DISP_MOSI        21
#define NB_HEAD_PIN_DISP_SCLK        47
#define NB_HEAD_PIN_DISP_DC          45
#define NB_HEAD_PIN_DISP_CS          (-1)
#define NB_HEAD_PIN_DISP_RST         (-1)
#define NB_HEAD_DISP_SPI_FREQ_KHZ    60000
#define NB_HEAD_DISP_WIDTH           240
#define NB_HEAD_DISP_HEIGHT          320

#define NB_HEAD_PIN_SD_CMD           38
#define NB_HEAD_PIN_SD_CLK           39
#define NB_HEAD_PIN_SD_DATA0         40
#define NB_HEAD_SD_BUS_WIDTH         1

#define NB_HEAD_PIN_UART0_TX         43
#define NB_HEAD_PIN_UART0_RX         44
#define NB_HEAD_PIN_STATUS_LED       48

_Static_assert(NB_HEAD_LINK_SPI_HOST != NB_HEAD_DISP_SPI_HOST,
               "head link/display must use different SPI hosts");
_Static_assert(NB_HEAD_PIN_LINK_CS != NB_HEAD_PIN_HEAD_IRQ,
               "head link CS/IRQ conflict");
_Static_assert(NB_HEAD_PIN_LINK_MISO != NB_HEAD_PIN_LINK_MOSI,
               "head link MISO/MOSI conflict");
_Static_assert(NB_HEAD_PIN_DISP_MOSI != NB_HEAD_PIN_LINK_MOSI,
               "head display/link MOSI conflict");
_Static_assert(NB_HEAD_PIN_DISP_SCLK != NB_HEAD_PIN_LINK_SCLK,
               "head display/link SCLK conflict");

#endif /* NB_HW_CONFIG_HEAD_H */
