idf_component_register(
    SRCS
        "touch_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        log
        freertos
        nb_hal
)
