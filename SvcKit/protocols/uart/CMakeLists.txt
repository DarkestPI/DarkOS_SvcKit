add_library(darkos_protocol_uart INTERFACE)
target_include_directories(darkos_protocol_uart INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(darkos_protocol_uart INTERFACE darkos_protocol_common)
add_library(DarkOS::Protocol::UART ALIAS darkos_protocol_uart)
