add_executable(serial_termios_test
    serial_termios_test.c
    ../shared/serial/serial_termios.c
)
target_compile_features(serial_termios_test PRIVATE c_std_17)
target_link_libraries(serial_termios_test PRIVATE serial_interfaces)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(serial_termios_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
set_target_properties(serial_termios_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(serial_termios_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()

add_test(NAME platform.serial_termios COMMAND serial_termios_test)
set_tests_properties(platform.serial_termios PROPERTIES TIMEOUT 10)
