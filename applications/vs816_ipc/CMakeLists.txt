cmake_minimum_required(VERSION 3.20)

project(vs816_ipc VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DARKOS_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "Path to DarkOS SDK")
include("${DARKOS_SDK_PATH}/cmake/DarkOS.cmake")

add_executable(vs816_ipc
    src/main.cpp
    ../common_ipc/src/AppOptions.cpp
    ../common_ipc/src/CaptureService.cpp
    ../common_ipc/src/IvaExample.cpp
    ../common_ipc/src/MediaSettings.cpp
    ../common_ipc/src/CameraEncodedVideo.cpp)

target_include_directories(vs816_ipc PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../common_ipc/src")
target_compile_features(vs816_ipc PRIVATE cxx_std_17)
target_compile_definitions(vs816_ipc PRIVATE
    DARKOS_CAMERA_ENCODED_MEDIA
    DARKOS_VIDEO_ONLY_MEDIA)
target_link_libraries(vs816_ipc PRIVATE
    DarkOS::SvcKit
    DarkOS::Protocol::RTSP
    stdc++fs)

if(DARKOS_ENABLE_OUTPUT_LAYOUT)
    set_target_properties(vs816_ipc PROPERTIES
        BUILD_RPATH "$ORIGIN/../lib"
        INSTALL_RPATH "$ORIGIN/../lib"
        BUILD_WITH_INSTALL_RPATH TRUE)
endif()

set(DARKOS_BOARD_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/boards/vs816_ipc_v1.json")
if(DARKOS_ENABLE_OUTPUT_LAYOUT)
    add_custom_command(TARGET vs816_ipc POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${DARKOS_BOARD_CONFIG}" "${DARKOS_OUTPUT_ROOT}/etc/board.json"
        COMMENT "Staging VS816 board configuration")
endif()
install(FILES "${DARKOS_BOARD_CONFIG}" DESTINATION etc RENAME board.json)
install(PROGRAMS scripts/start_virtual_video.sh DESTINATION bin)

if(DARKOS_ENABLE_OUTPUT_LAYOUT)
    add_custom_command(TARGET vs816_ipc POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/start_virtual_video.sh"
            "${DARKOS_OUTPUT_ROOT}/bin/start_virtual_video.sh"
        COMMENT "Staging VS816 virtual-video launcher")
endif()

darkos_add_application(vs816_ipc)
