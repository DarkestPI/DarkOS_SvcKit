cmake_minimum_required(VERSION 3.20)

project(generic_ipc VERSION 0.1.0 LANGUAGES C CXX)

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

add_executable(generic_ipc
    src/main.cpp
)

target_compile_features(generic_ipc PRIVATE cxx_std_17)

target_link_libraries(generic_ipc PRIVATE DarkOS::SvcKit)

set(DARKOS_BOARD "ubuntu_x86_64_host" CACHE STRING
    "Board configuration packaged with generic_ipc")
set(DARKOS_BOARD_CONFIG
    "${CMAKE_CURRENT_SOURCE_DIR}/boards/${DARKOS_BOARD}.json")
if(NOT EXISTS "${DARKOS_BOARD_CONFIG}")
    message(FATAL_ERROR
        "Unknown DARKOS_BOARD='${DARKOS_BOARD}': ${DARKOS_BOARD_CONFIG} does not exist")
endif()

if(DARKOS_ENABLE_OUTPUT_LAYOUT)
    add_custom_command(TARGET generic_ipc POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${DARKOS_BOARD_CONFIG}"
            "${DARKOS_OUTPUT_ROOT}/etc/board.json"
        COMMENT "Staging ${DARKOS_BOARD} board configuration")
endif()

install(FILES "${DARKOS_BOARD_CONFIG}"
    DESTINATION etc
    RENAME board.json)

darkos_add_application(generic_ipc)
