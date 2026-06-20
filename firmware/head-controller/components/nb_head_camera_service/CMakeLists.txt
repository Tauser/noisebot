idf_component_register(
    SRCS "nb_head_camera_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_common
        freertos
        log
        nb_inter_mcu_protocol
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
