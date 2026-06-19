idf_component_register(
    SRCS
        "presence_semantic_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        log
        infra
)
