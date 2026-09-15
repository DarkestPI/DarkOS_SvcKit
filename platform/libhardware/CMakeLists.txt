find_package(Threads REQUIRED)

add_library(hardware_base STATIC
    hardware.c
)

target_include_directories(hardware_base
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_features(hardware_base PUBLIC c_std_17)
target_compile_definitions(hardware_base PRIVATE
    DARKOS_DEFAULT_HAL_VARIANT="${DARKOS_HAL_DEFAULT_VARIANT}")
target_link_libraries(hardware_base PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
set_target_properties(hardware_base PROPERTIES POSITION_INDEPENDENT_CODE ON)
darkos_prepare_target_output(hardware_base)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hardware_base PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(DARKOS_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(tests)
endif()
