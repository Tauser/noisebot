idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_system
        esp_timer
        driver
        freertos
        log
        nb_head_hal
        nb_inter_mcu_protocol
)
