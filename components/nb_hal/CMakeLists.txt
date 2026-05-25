set(NB_HAL_REQUIRES
    log
    fatfs
    sdmmc
    esp_common
    esp_psram
    driver
    esp_driver_i2s
    esp_driver_i2c
    led_strip
    LovyanGFX
)

if(CONFIG_NB_CAMERA_DIAG_ENABLED)
    list(APPEND NB_HAL_REQUIRES
        esp_video
    )
endif()

idf_component_register(
    SRCS
        "sd_hal.c"
        "display_hal.cpp"
        "led_hal.c"
        "touch_hal.c"
        "servo_hal.c"
        "audio_hal.c"
        "i2c_hal.c"
        "camera_hal.c"
    INCLUDE_DIRS "."
    REQUIRES ${NB_HAL_REQUIRES}
)

if(CONFIG_NB_CAMERA_DIAG_ENABLED)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NB_CAMERA_DIAG_ENABLED=1)
endif()
