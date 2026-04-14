idf_component_register(
    SRCS
        "motion_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        nb_hal
        freertos
        log
        esp_common
)
