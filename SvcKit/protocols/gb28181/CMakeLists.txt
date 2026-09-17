add_library(darkos_protocol_gb28181 INTERFACE)
target_include_directories(darkos_protocol_gb28181 INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(darkos_protocol_gb28181 INTERFACE darkos_protocol_common)
add_library(DarkOS::Protocol::GB28181 ALIAS darkos_protocol_gb28181)
