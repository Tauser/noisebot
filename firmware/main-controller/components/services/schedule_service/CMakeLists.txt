idf_component_register(
    SRCS
        "schedule_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        log
        infra
)
