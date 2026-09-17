add_library(darkos_protocol_common STATIC
    src/H264RtpPacketizer.cpp)
target_include_directories(darkos_protocol_common PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(darkos_protocol_common PUBLIC cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(darkos_protocol_common PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
darkos_prepare_target_output(darkos_protocol_common)
add_library(DarkOS::Protocol::Common ALIAS darkos_protocol_common)
