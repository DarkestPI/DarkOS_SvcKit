# Platform 可以被任意 application 作为子目录引入，不能在这里调用 project()。
# libhardware 尚未落地时保留同名骨架目标，接口包仍可参与依赖建模。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libhardware/CMakeLists.txt")
    add_subdirectory(libhardware)
else()
    add_library(hardware_base INTERFACE)
endif()

add_library(DarkOS::HardwareBase ALIAS hardware_base)

add_subdirectory(interfaces)

add_library(darkos_platform INTERFACE)
target_link_libraries(darkos_platform INTERFACE hardware_interfaces)
add_library(DarkOS::Platform ALIAS darkos_platform)

if(DARKOS_BUILD_HAL_IMPLEMENTATIONS)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libhardware/CMakeLists.txt")
        message(FATAL_ERROR
            "DARKOS_BUILD_HAL_IMPLEMENTATIONS=ON requires platform/libhardware")
    endif()

    if(CMAKE_CROSSCOMPILING)
        add_subdirectory(rockchip)
        target_link_libraries(darkos_platform INTERFACE hal_rockchip)
    else()
        add_subdirectory(host_x86)
        target_link_libraries(darkos_platform INTERFACE hal_host_x86)
    endif()
endif()

if(DARKOS_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()
