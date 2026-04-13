idf_component_register(
    SRCS
        "sd_hal.c"
        "display_hal.cpp"
        "led_hal.c"
        "touch_hal.c"
    INCLUDE_DIRS "."
    REQUIRES
        log
        fatfs
        sdmmc
        esp_common
        esp_psram
        driver
        led_strip
        LovyanGFX
)
