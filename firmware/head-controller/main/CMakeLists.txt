idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_system
        esp_timer
        freertos
        log
        nb_inter_mcu_protocol
)
