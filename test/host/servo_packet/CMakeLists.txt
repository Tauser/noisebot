add_executable(test_servo_packet test_servo_packet.c)
target_compile_options(test_servo_packet PRIVATE -Wall -Wextra -Werror)
set_target_properties(test_servo_packet PROPERTIES C_STANDARD 17)
add_test(NAME servo_packet COMMAND test_servo_packet)
