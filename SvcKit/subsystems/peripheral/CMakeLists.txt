add_library(svckit_peripheral STATIC src/SerialPort.cpp)
target_include_directories(svckit_peripheral PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(svckit_peripheral PUBLIC cxx_std_17)
target_link_libraries(svckit_peripheral
    PUBLIC svckit_base
    PRIVATE hardware_base serial_interfaces)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(svckit_peripheral PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
darkos_prepare_target_output(svckit_peripheral)

if(DARKOS_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING AND TARGET hal_host_x86)
    add_executable(svc_peripheral_serial_test tests/serial_port_test.cpp)
    target_link_libraries(svc_peripheral_serial_test PRIVATE svckit_peripheral)
    target_compile_features(svc_peripheral_serial_test PRIVATE cxx_std_17)
    add_dependencies(svc_peripheral_serial_test hal_host_x86)
    add_test(NAME svc_peripheral.serial
        COMMAND "${CMAKE_COMMAND}" -E env
            "DARKOS_HAL_VARIANT=host_x86"
            "DARKOS_HAL_LIBRARY_PATH=$<TARGET_FILE_DIR:hal_host_x86>"
            "$<TARGET_FILE:svc_peripheral_serial_test>")
    set_tests_properties(svc_peripheral.serial PROPERTIES TIMEOUT 5)
    if(TARGET svckit_host_tests)
        add_dependencies(svckit_host_tests svc_peripheral_serial_test)
    endif()
endif()
