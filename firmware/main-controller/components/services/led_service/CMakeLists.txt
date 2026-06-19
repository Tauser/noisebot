idf_component_register(
    SRCS
        "led_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        log
        freertos
        nb_hal
)
