idf_component_register(
    SRCS
        "touch_semantic_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        log
        infra
        touch_service
        synth_service
)
