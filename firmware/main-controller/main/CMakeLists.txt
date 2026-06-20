idf_component_register(
    SRCS "servo_test.c" "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        infra
        visual_state_facade
        audio_service
        nb_hal
        nb_main_hal
        driver
        freertos
        log
)
