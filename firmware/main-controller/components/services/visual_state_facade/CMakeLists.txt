idf_component_register(
    SRCS "visual_state_facade.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_common
        freertos
        nb_inter_mcu_protocol
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
