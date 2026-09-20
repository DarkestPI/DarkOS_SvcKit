cmake_minimum_required(VERSION 3.20)

project(rv1126b_ipc VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DARKOS_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "Path to DarkOS SDK")

if(NOT EXISTS "${DARKOS_SDK_PATH}/cmake/DarkOS.cmake")
    message(FATAL_ERROR
        "Invalid DARKOS_SDK_PATH: ${DARKOS_SDK_PATH}\n"
        "Cannot find cmake/DarkOS.cmake")
endif()

include("${DARKOS_SDK_PATH}/cmake/DarkOS.cmake")

add_executable(rv1126b_ipc
    src/main.cpp
    # RV1126B 使用与 generic_ipc 相同的采集、存储和 RTSP 服务编排。
    ../generic_ipc/src/AppOptions.cpp
    ../generic_ipc/src/CaptureService.cpp
    ../generic_ipc/src/IvaExample.cpp
    ../generic_ipc/src/MediaSettings.cpp
    ../generic_ipc/src/CameraEncodedVideo.cpp
)

target_include_directories(rv1126b_ipc PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../generic_ipc/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_features(rv1126b_ipc PRIVATE cxx_std_17)
target_compile_definitions(rv1126b_ipc PRIVATE DARKOS_CAMERA_ENCODED_MEDIA)

target_link_libraries(rv1126b_ipc PRIVATE
    DarkOS::SvcKit
    DarkOS::Protocol::RTSP)

# 运行时库随 output/bin 一起发布到 output/lib；不要把开发机 SDK 的绝对路径
# 写进板端程序的运行时搜索路径。
if(DARKOS_ENABLE_OUTPUT_LAYOUT)
    set_target_properties(rv1126b_ipc PROPERTIES
        BUILD_RPATH "$ORIGIN/../lib"
        INSTALL_RPATH "$ORIGIN/../lib"
        BUILD_WITH_INSTALL_RPATH TRUE)
endif()

set(DARKOS_BOARD "rv1126b_ipc_v1" CACHE STRING
    "Board configuration packaged with rv1126b_ipc")
set(DARKOS_BOARD_CONFIG
    "${CMAKE_CURRENT_SOURCE_DIR}/boards/${DARKOS_BOARD}.json")
if(NOT EXISTS "${DARKOS_BOARD_CONFIG}")
    message(FATAL_ERROR
        "Unknown DARKOS_BOARD='${DARKOS_BOARD}': ${DARKOS_BOARD_CONFIG} does not exist")
endif()

if(DARKOS_ENABLE_OUTPUT_LAYOUT)
    add_custom_command(TARGET rv1126b_ipc POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${DARKOS_BOARD_CONFIG}"
            "${DARKOS_OUTPUT_ROOT}/etc/board.json"
        COMMENT "Staging ${DARKOS_BOARD} board configuration")
endif()

install(FILES "${DARKOS_BOARD_CONFIG}"
    DESTINATION etc
    RENAME board.json)

darkos_add_application(rv1126b_ipc)
