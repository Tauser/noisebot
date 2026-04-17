idf_component_register(
    SRCS
        "circadian_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        log
        infra
        conductor
)
