idf_component_register(
    SRCS "nb_head_i2c_hal.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_i2c
        log
        nb_head_hal
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
