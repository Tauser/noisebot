idf_component_register(
    SRCS
        "sd_hal.c"
        "display_hal.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        log
        fatfs
        sdmmc
        esp_common
        esp_psram
        driver
        LovyanGFX
)
