idf_component_register(
    SRCS "nb_head_status_led.c"
    INCLUDE_DIRS "include"
    REQUIRES driver led_strip
)
