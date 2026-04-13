/*
 * display_lgfx_config.hpp — Configuração LovyanGFX para os painéis do NoiseBot
 *
 * Define duas classes de dispositivo:
 *   LGFX_ST7789  — 240×240, CS no GND, sem RST, sem BL (painel atual)
 *   LGFX_ILI9342 — 320×240, CS/RST/BL via GPIO (painel futuro)
 *
 * O alias NB_LGFX aponta para o driver ativo:
 *   default          → LGFX_ST7789
 *   NB_DISP_DRIVER_ILI9342 definido → LGFX_ILI9342
 *
 * Para trocar de painel: definir NB_DISP_DRIVER_ILI9342 no CMakeLists.txt
 * do nb_hal via target_compile_definitions e preencher os pinos TBD em
 * nb_hw_config.h.
 */

#pragma once

#include <LovyanGFX.hpp>
#include "nb_hw_config.h"

/* ── ST7789 240×240 ──────────────────────────────────────────────────────── */

class LGFX_ST7789 : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_SPI      _bus;

public:
    LGFX_ST7789()
    {
        /* Barramento SPI2 */
        {
            auto cfg        = _bus.config();
            cfg.spi_host    = NB_DISP_SPI_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = NB_DISP_SPI_FREQ_KHZ * 1000;
            cfg.freq_read   = 16000000;
            cfg.pin_sclk    = NB_DISP_PIN_SCLK;
            cfg.pin_mosi    = NB_DISP_PIN_MOSI;
            cfg.pin_miso    = NB_DISP_PIN_MISO;
            cfg.pin_dc      = NB_DISP_PIN_DC;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }

        /* Painel ST7789 */
        {
            auto cfg            = _panel.config();
            cfg.pin_cs          = NB_DISP_ST7789_PIN_CS;   /* -1 = CS no GND */
            cfg.pin_rst         = NB_DISP_ST7789_PIN_RST;  /* -1 = sem RST   */
            cfg.pin_busy        = -1;
            /* ST7789 320x240 landscape nativo (sem rotação de memória). */
            cfg.memory_width    = 240;
            cfg.memory_height   = 320;
            cfg.panel_width     = 240;
            cfg.panel_height    = 320;
            cfg.offset_x        = 0;
            cfg.offset_y        = 0;
            cfg.offset_rotation = 1;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable        = false;
            cfg.invert          = true;
            cfg.rgb_order       = false;
            cfg.dlen_16bit      = false;
            cfg.bus_shared      = false;
            _panel.config(cfg);
        }

        setPanel(&_panel);
    }
};

/* ── ILI9342 320×240 ─────────────────────────────────────────────────────── */

class LGFX_ILI9342 : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9342 _panel;
    lgfx::Bus_SPI        _bus;
    lgfx::Light_PWM      _backlight;

public:
    LGFX_ILI9342()
    {
        /* Barramento SPI2 (mesmo host, pinos SCLK/MOSI/DC compartilhados) */
        {
            auto cfg        = _bus.config();
            cfg.spi_host    = NB_DISP_SPI_HOST;
            cfg.spi_mode    = 3;                            /* ILI9342: mode 0 */
            cfg.freq_write  = NB_DISP_SPI_FREQ_KHZ * 1000;
            cfg.freq_read   = 16000000;
            cfg.pin_sclk    = NB_DISP_PIN_SCLK;
            cfg.pin_mosi    = NB_DISP_PIN_MOSI;
            cfg.pin_miso    = NB_DISP_PIN_MISO;
            cfg.pin_dc      = NB_DISP_PIN_DC;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }

        /* Painel ILI9342 */
        {
            auto cfg            = _panel.config();
            cfg.pin_cs          = NB_DISP_ILI9342_PIN_CS;
            cfg.pin_rst         = NB_DISP_ILI9342_PIN_RST;
            cfg.pin_busy        = -1;
            cfg.memory_width    = NB_DISP_ILI9342_WIDTH;
            cfg.memory_height   = NB_DISP_ILI9342_HEIGHT;
            cfg.panel_width     = NB_DISP_ILI9342_WIDTH;
            cfg.panel_height    = NB_DISP_ILI9342_HEIGHT;
            cfg.offset_x        = 0;
            cfg.offset_y        = 0;
            cfg.offset_rotation = 0;
            cfg.readable        = false;
            cfg.invert          = false;
            cfg.rgb_order       = false;
            cfg.dlen_16bit      = false;
            cfg.bus_shared      = false;
            _panel.config(cfg);
        }

        /* Backlight LEDC (só configurado se pino definido) */
#if NB_DISP_ILI9342_PIN_BL >= 0
        {
            auto cfg        = _backlight.config();
            cfg.pin_bl      = NB_DISP_ILI9342_PIN_BL;
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = NB_DISP_LEDC_CHANNEL;
            _backlight.config(cfg);
            _panel.setLight(&_backlight);
        }
#endif

        setPanel(&_panel);
    }
};

/* ── Alias do driver ativo ───────────────────────────────────────────────── */

#if defined(NB_DISP_DRIVER_ILI9342)
using NB_LGFX = LGFX_ILI9342;
#else
using NB_LGFX = LGFX_ST7789;
#endif
