idf_component_register(
    SRCS
        "sd_hal.c"
    INCLUDE_DIRS "."
    REQUIRES
        log
        fatfs
        sdmmc
        esp_common
)
