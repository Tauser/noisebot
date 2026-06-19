idf_component_register(
    SRCS "nb_head_link_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_system
        esp_timer
        freertos
        log
        head_link_transport
        nb_head_hal
        nb_inter_mcu_protocol
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
