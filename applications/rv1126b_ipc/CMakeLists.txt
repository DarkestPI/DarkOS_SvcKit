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
)

target_compile_features(rv1126b_ipc PRIVATE cxx_std_17)

target_link_libraries(rv1126b_ipc PRIVATE DarkOS::SvcKit)

darkos_add_application(rv1126b_ipc)
