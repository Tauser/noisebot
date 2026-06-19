idf_component_register(
    SRCS
        "persona_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        log
        nvs_flash
        infra
        long_term_memory
)
