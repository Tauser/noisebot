add_executable(
    test_inter_mcu_protocol
    test_inter_mcu_protocol.c
    ../../../../shared/components/nb_inter_mcu_protocol/nb_inter_mcu_protocol.c
)
target_include_directories(
    test_inter_mcu_protocol
    PRIVATE ../../../../shared/components/nb_inter_mcu_protocol/include
)
target_compile_options(test_inter_mcu_protocol PRIVATE -Wall -Wextra -Werror)
set_target_properties(test_inter_mcu_protocol PROPERTIES C_STANDARD 17)
add_test(NAME inter_mcu_protocol COMMAND test_inter_mcu_protocol)
