idf_component_register(
    SRCS
        "time_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        log
        nvs_flash
        lwip
        infra
)
