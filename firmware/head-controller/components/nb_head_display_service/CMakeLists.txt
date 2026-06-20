idf_component_register(
    SRCS "nb_head_display_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_common
        freertos
        log
        nb_head_display_hal
        nb_head_hal
        nb_inter_mcu_protocol
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
