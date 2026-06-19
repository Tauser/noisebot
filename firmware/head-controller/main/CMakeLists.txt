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
        nb_head_display_service
        nb_head_link_service
        nb_inter_mcu_protocol
)
