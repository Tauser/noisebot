set(NB_HAL_REQUIRES
    log
    esp_common
    esp_psram
    driver
    esp_driver_i2s
    esp_driver_i2c
    nb_main_hal
)

set(NB_HAL_SRCS
    "led_hal.c"
    "touch_hal.c"
    "servo_hal.c"
    "audio_hal.c"
    "board_caps.c"
    "i2c_hal.c"
)

if(CONFIG_NB_BOARD_PROFILE_WAVESHARE)
    list(APPEND NB_HAL_SRCS
        "display_hal_waveshare_stub.cpp"
        "sd_hal_waveshare_stub.c"
        "camera_hal_waveshare_stub.c"
    )
else()
    list(APPEND NB_HAL_REQUIRES
        fatfs
        LovyanGFX
    )
    list(APPEND NB_HAL_SRCS
        "sd_hal.c"
        "display_hal.cpp"
        "camera_hal.c"
    )
    if(CONFIG_NB_CAMERA_DIAG_ENABLED)
        list(APPEND NB_HAL_REQUIRES esp_video)
    endif()
endif()

idf_component_register(
    SRCS ${NB_HAL_SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${NB_HAL_REQUIRES}
)

if(CONFIG_NB_CAMERA_DIAG_ENABLED AND NOT CONFIG_NB_BOARD_PROFILE_WAVESHARE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NB_CAMERA_DIAG_ENABLED=1)
endif()
