add_library(darkos_protocol_rtsp STATIC src/RtspServer.cpp)
target_include_directories(darkos_protocol_rtsp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(darkos_protocol_rtsp PUBLIC cxx_std_17)
target_link_libraries(darkos_protocol_rtsp
    PUBLIC svckit_media svckit_networking
    PRIVATE darkos_protocol_common)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(darkos_protocol_rtsp PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
darkos_prepare_target_output(darkos_protocol_rtsp)
add_library(DarkOS::Protocol::RTSP ALIAS darkos_protocol_rtsp)

if(DARKOS_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    add_executable(protocol_rtsp_test tests/rtsp_test.cpp)
    target_link_libraries(protocol_rtsp_test PRIVATE darkos_protocol_rtsp)
    target_compile_features(protocol_rtsp_test PRIVATE cxx_std_17)
    add_test(NAME protocol.rtsp_v2 COMMAND protocol_rtsp_test)
    set_tests_properties(protocol.rtsp_v2 PROPERTIES TIMEOUT 10)
    if(TARGET svckit_host_tests)
        add_dependencies(svckit_host_tests protocol_rtsp_test)
    endif()
endif()
