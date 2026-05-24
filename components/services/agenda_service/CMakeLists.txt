idf_component_register(
    SRCS
        "agenda_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        log
        nvs_flash
        json
        infra
        time_service
)
