#pragma once

#include <LovyanGFX.hpp>

#include "nb_hw_config_head.h"

class NBHeadLGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 panel_;
    lgfx::Bus_SPI bus_;

public:
    NBHeadLGFX()
    {
        {
            auto cfg = bus_.config();
            cfg.spi_host = NB_HEAD_DISP_SPI_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = NB_HEAD_DISP_SPI_FREQ_KHZ * 1000;
            cfg.freq_read = 16000000;
            cfg.pin_sclk = NB_HEAD_PIN_DISP_SCLK;
            cfg.pin_mosi = NB_HEAD_PIN_DISP_MOSI;
            cfg.pin_miso = -1;
            cfg.pin_dc = NB_HEAD_PIN_DISP_DC;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            bus_.config(cfg);
            panel_.setBus(&bus_);
        }

        {
            auto cfg = panel_.config();
            cfg.pin_cs = NB_HEAD_PIN_DISP_CS;
            cfg.pin_rst = -1;
            cfg.pin_busy = -1;
            cfg.memory_width = NB_HEAD_DISP_WIDTH;
            cfg.memory_height = NB_HEAD_DISP_HEIGHT;
            cfg.panel_width = NB_HEAD_DISP_WIDTH;
            cfg.panel_height = NB_HEAD_DISP_HEIGHT;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.offset_rotation = 1;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits = 1;
            cfg.readable = false;
            cfg.invert = true;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;
            panel_.config(cfg);
        }

        setPanel(&panel_);
    }
};
