idf_component_register(
    SRCS "servo_test.c" "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        infra
        audio_service
        nb_hal
        driver
        freertos
        log
)
