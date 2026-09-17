add_library(svckit_networking STATIC
    src/socket/address.cpp
    src/socket/tcp_socket.cpp
    src/socket/udp_socket.cpp
    src/connection/tcp_server.cpp
    src/core/network_manager.cpp
    src/interface/wifi_manager.cpp
)

target_include_directories(svckit_networking PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
    $<INSTALL_INTERFACE:include>)
target_compile_features(svckit_networking PUBLIC cxx_std_17)
target_link_libraries(svckit_networking
    PUBLIC svckit_base
    PRIVATE hardware_base wifi_interfaces)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(svckit_networking PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()

darkos_prepare_target_output(svckit_networking)

if(DARKOS_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    add_executable(svc_network_socket_test tests/network_socket_test.cpp)
    target_link_libraries(svc_network_socket_test PRIVATE svckit_networking)
    target_compile_features(svc_network_socket_test PRIVATE cxx_std_17)
    add_test(NAME svc_network.socket COMMAND svc_network_socket_test)
    set_tests_properties(svc_network.socket PROPERTIES TIMEOUT 5)
    add_executable(svc_network_async_test tests/network_async_test.cpp)
    target_link_libraries(svc_network_async_test PRIVATE svckit_networking)
    target_compile_features(svc_network_async_test PRIVATE cxx_std_17)
    add_test(NAME svc_network.async COMMAND svc_network_async_test)
    set_tests_properties(svc_network.async PROPERTIES TIMEOUT 5)
    add_executable(svc_network_manager_test tests/network_manager_test.cpp)
    target_link_libraries(svc_network_manager_test PRIVATE svckit_networking)
    target_compile_features(svc_network_manager_test PRIVATE cxx_std_17)
    add_test(NAME svc_network.manager COMMAND svc_network_manager_test)
    set_tests_properties(svc_network.manager PROPERTIES TIMEOUT 5)
    if(TARGET hal_host_x86)
        add_executable(svc_wifi_manager_test tests/wifi_manager_test.cpp)
        target_link_libraries(svc_wifi_manager_test PRIVATE svckit_networking)
        target_compile_features(svc_wifi_manager_test PRIVATE cxx_std_17)
        add_dependencies(svc_wifi_manager_test hal_host_x86)
        add_test(NAME svc_network.wifi
            COMMAND "${CMAKE_COMMAND}" -E env
                "DARKOS_HAL_VARIANT=host_x86"
                "DARKOS_HAL_LIBRARY_PATH=$<TARGET_FILE_DIR:hal_host_x86>"
                "$<TARGET_FILE:svc_wifi_manager_test>")
        set_tests_properties(svc_network.wifi PROPERTIES TIMEOUT 5)
    endif()
    if(TARGET svckit_host_tests)
        add_dependencies(svckit_host_tests
            svc_network_socket_test svc_network_async_test
            svc_network_manager_test)
        if(TARGET svc_wifi_manager_test)
            add_dependencies(svckit_host_tests svc_wifi_manager_test)
        endif()
    endif()
endif()
