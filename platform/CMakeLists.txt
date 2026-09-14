# Platform 由 cmake/DarkOS.cmake 引入，也允许 Application 通过
# add_subdirectory() 单独嵌入。因此这里不调用 project()，也不设置全局编译参数
# 或应用侧策略。

set(DARKOS_SUPPORTED_PLATFORMS
    ubuntu_x86_64
    allwinner
    artosyn
    ingenic
    novatek
    rockchip
    sunplus
)

# 每次构建只选择一个平台实现。auto 采用保守的识别规则：Rockchip SDK 变量用于
# 识别当前板端工具链；Linux x86-64 本机构建使用主机参考实现。其他交叉编译目标
# 必须显式指定平台，避免仅根据 CMAKE_CROSSCOMPILING 误选为 Rockchip。
set(DARKOS_PLATFORM "auto" CACHE STRING
    "Platform HAL implementation (auto or one of: ${DARKOS_SUPPORTED_PLATFORMS})")
set_property(CACHE DARKOS_PLATFORM PROPERTY STRINGS auto ${DARKOS_SUPPORTED_PLATFORMS})

function(_darkos_detect_platform result)
    if(DEFINED DARKOS_RV1126B_SDK OR DEFINED DARKOS_RV1126B_MEDIA_ROOT)
        set(platform rockchip)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux"
           AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        set(platform ubuntu_x86_64)
    else()
        set(platform "")
    endif()

    set(${result} "${platform}" PARENT_SCOPE)
endfunction()

if(DARKOS_PLATFORM STREQUAL "auto")
    _darkos_detect_platform(DARKOS_PLATFORM_RESOLVED)
else()
    set(DARKOS_PLATFORM_RESOLVED "${DARKOS_PLATFORM}")
endif()

if(DARKOS_PLATFORM_RESOLVED
   AND NOT DARKOS_PLATFORM_RESOLVED IN_LIST DARKOS_SUPPORTED_PLATFORMS)
    message(FATAL_ERROR
        "Unsupported DARKOS_PLATFORM='${DARKOS_PLATFORM_RESOLVED}'.\n"
        "Supported names: ${DARKOS_SUPPORTED_PLATFORMS}")
endif()

set(DARKOS_PLATFORM_RESOLVED "${DARKOS_PLATFORM_RESOLVED}" CACHE INTERNAL
    "Resolved DarkOS platform implementation" FORCE)

# 即使没有启用具体 HAL 实现，也始终建立公共 HAL 依赖图。当前仓库没有
# libhardware 加载器源码，因此先使用 INTERFACE 骨架目标；加载器补齐后会自动
# 改为使用 libhardware 子目录提供的真实目标。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libhardware/CMakeLists.txt")
    add_subdirectory(libhardware)
else()
    add_library(hardware_base INTERFACE)
endif()

if(NOT TARGET hardware_base)
    message(FATAL_ERROR
        "platform/libhardware must define the 'hardware_base' target")
endif()

add_library(DarkOS::HardwareBase ALIAS hardware_base)

add_subdirectory(interfaces)

add_library(darkos_platform INTERFACE)
target_link_libraries(darkos_platform INTERFACE hardware_interfaces)
add_library(DarkOS::Platform ALIAS darkos_platform)

if(DARKOS_BUILD_HAL_IMPLEMENTATIONS)
    if(NOT DARKOS_PLATFORM_RESOLVED)
        message(FATAL_ERROR
            "Cannot auto-detect the target platform for "
            "CMAKE_SYSTEM_NAME='${CMAKE_SYSTEM_NAME}', "
            "CMAKE_SYSTEM_PROCESSOR='${CMAKE_SYSTEM_PROCESSOR}'.\n"
            "Set -DDARKOS_PLATFORM=<name> explicitly.")
    endif()

    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libhardware/CMakeLists.txt")
        message(FATAL_ERROR
            "DARKOS_BUILD_HAL_IMPLEMENTATIONS=ON requires "
            "platform/libhardware/CMakeLists.txt, but the HAL loader is not "
            "present in this checkout.")
    endif()

    set(platform_dir "${CMAKE_CURRENT_SOURCE_DIR}/${DARKOS_PLATFORM_RESOLVED}")
    if(NOT EXISTS "${platform_dir}/CMakeLists.txt")
        message(FATAL_ERROR
            "Platform '${DARKOS_PLATFORM_RESOLVED}' is a reserved platform "
            "name, but its implementation is not available yet: ${platform_dir}")
    endif()

    add_subdirectory("${DARKOS_PLATFORM_RESOLVED}")

    # 现有实现目标名早于本次目录改名。旧名称的兼容映射集中保留在这里；后续新增
    # 平台统一使用 hal_<platform> 作为 CMake 目标名。
    if(DARKOS_PLATFORM_RESOLVED STREQUAL "ubuntu_x86_64")
        set(platform_hal_target hal_host_x86)
    else()
        set(platform_hal_target "hal_${DARKOS_PLATFORM_RESOLVED}")
    endif()

    if(NOT TARGET "${platform_hal_target}")
        message(FATAL_ERROR
            "${DARKOS_PLATFORM_RESOLVED}/CMakeLists.txt must define target "
            "'${platform_hal_target}'")
    endif()

    target_link_libraries(darkos_platform INTERFACE "${platform_hal_target}")
    message(STATUS "DarkOS platform implementation: ${DARKOS_PLATFORM_RESOLVED}")
elseif(DARKOS_PLATFORM_RESOLVED)
    message(STATUS
        "DarkOS platform: ${DARKOS_PLATFORM_RESOLVED} (interfaces only)")
else()
    message(STATUS "DarkOS platform: interfaces only")
endif()

if(DARKOS_BUILD_EXAMPLES
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()
