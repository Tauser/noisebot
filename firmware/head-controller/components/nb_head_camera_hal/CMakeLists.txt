idf_component_register(
    SRCS "nb_head_camera_hal.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_psram
        esp_video
        freertos
        heap
        log
        nb_head_hal
        nb_head_i2c_hal
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
