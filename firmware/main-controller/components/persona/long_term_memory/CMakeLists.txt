idf_component_register(
    SRCS
        "long_term_memory.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        log
        esp_rom
        infra
)
