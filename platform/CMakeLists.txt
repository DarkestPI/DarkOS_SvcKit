# Platform 由 cmake/DarkOS.cmake 引入，也允许 Application 通过 add_subdirectory()
# 单独嵌入。因此这里不调用 project()，也不设置应用侧的全局编译策略。

set(DARKOS_SUPPORTED_REFERENCE_PLATFORMS ubuntu_x86_64)
set(DARKOS_SUPPORTED_VENDORS
    rockchip
    allwinner
    artosyn
    gokemicro
    ingenic
    novatek
    visinextek
    sunplus)

# Reference Platform 用于宿主机模拟；真实芯片使用 Vendor + SoC 两级选择。
# DARKOS_PLATFORM=rockchip 的旧写法暂时兼容，并会映射到 Vendor 模式。
set(DARKOS_PLATFORM "auto" CACHE STRING
    "Reference HAL platform (auto or ubuntu_x86_64; legacy vendor names accepted)")
set_property(CACHE DARKOS_PLATFORM PROPERTY STRINGS
    auto ${DARKOS_SUPPORTED_REFERENCE_PLATFORMS})
set(DARKOS_VENDOR "" CACHE STRING
    "HAL silicon vendor (for example: rockchip)")
set_property(CACHE DARKOS_VENDOR PROPERTY STRINGS "" ${DARKOS_SUPPORTED_VENDORS})
set(DARKOS_SOC "" CACHE STRING
    "HAL SoC model selected within DARKOS_VENDOR (for example: rv1126b)")

set(resolved_platform "${DARKOS_PLATFORM}")
set(resolved_vendor "${DARKOS_VENDOR}")
set(resolved_soc "${DARKOS_SOC}")

# 兼容旧工程：此前 DARKOS_PLATFORM 同时承担 vendor 的含义。
if(resolved_platform IN_LIST DARKOS_SUPPORTED_VENDORS)
    if(resolved_vendor AND NOT resolved_vendor STREQUAL resolved_platform)
        message(FATAL_ERROR
            "DARKOS_PLATFORM='${resolved_platform}' conflicts with "
            "DARKOS_VENDOR='${resolved_vendor}'")
    endif()
    set(resolved_vendor "${resolved_platform}")
    set(resolved_platform "auto")
    message(DEPRECATION
        "DARKOS_PLATFORM=<vendor> is deprecated; use "
        "DARKOS_VENDOR=<vendor> and DARKOS_SOC=<soc>")
endif()

# 保守自动识别：只识别仓库已有且能可靠确认的工具链。
if(NOT resolved_vendor AND
   (DEFINED DARKOS_RV1126B_SDK OR DEFINED DARKOS_RV1126B_MEDIA_ROOT))
    set(resolved_vendor rockchip)
endif()
if(resolved_vendor STREQUAL "rockchip" AND NOT resolved_soc AND
   (DEFINED DARKOS_RV1126B_SDK OR DEFINED DARKOS_RV1126B_MEDIA_ROOT))
    set(resolved_soc rv1126b)
endif()

if(resolved_vendor)
    if(NOT resolved_platform STREQUAL "auto" AND resolved_platform)
        message(FATAL_ERROR
            "DARKOS_PLATFORM='${resolved_platform}' cannot be combined with "
            "DARKOS_VENDOR='${resolved_vendor}'")
    endif()
    if(NOT resolved_vendor IN_LIST DARKOS_SUPPORTED_VENDORS)
        message(FATAL_ERROR
            "Unsupported DARKOS_VENDOR='${resolved_vendor}'.\n"
            "Known vendors: ${DARKOS_SUPPORTED_VENDORS}")
    endif()
    if(NOT resolved_soc)
        message(FATAL_ERROR
            "DARKOS_SOC is required when DARKOS_VENDOR='${resolved_vendor}'")
    endif()
    set(resolved_platform "")
elseif(resolved_soc)
    message(FATAL_ERROR
        "DARKOS_VENDOR is required when DARKOS_SOC='${resolved_soc}'")
elseif(resolved_platform STREQUAL "auto")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
       CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        set(resolved_platform ubuntu_x86_64)
    else()
        set(resolved_platform "")
    endif()
endif()

if(resolved_platform AND
   NOT resolved_platform IN_LIST DARKOS_SUPPORTED_REFERENCE_PLATFORMS)
    message(FATAL_ERROR
        "Unsupported DARKOS_PLATFORM='${resolved_platform}'.\n"
        "Reference platforms: ${DARKOS_SUPPORTED_REFERENCE_PLATFORMS}")
endif()

set(DARKOS_PLATFORM_RESOLVED "${resolved_platform}" CACHE INTERNAL
    "Resolved DarkOS reference platform" FORCE)
set(DARKOS_VENDOR_RESOLVED "${resolved_vendor}" CACHE INTERNAL
    "Resolved DarkOS HAL vendor" FORCE)
set(DARKOS_SOC_RESOLVED "${resolved_soc}" CACHE INTERNAL
    "Resolved DarkOS HAL SoC" FORCE)

if(DARKOS_VENDOR_RESOLVED)
    set(DARKOS_HAL_DEFAULT_VARIANT
        "${DARKOS_VENDOR_RESOLVED}.${DARKOS_SOC_RESOLVED}")
elseif(DARKOS_PLATFORM_RESOLVED STREQUAL "ubuntu_x86_64")
    set(DARKOS_HAL_DEFAULT_VARIANT host_x86)
else()
    set(DARKOS_HAL_DEFAULT_VARIANT "")
endif()

# libhardware 是应用唯一直接链接的 HAL 运行时部分；实现保持独立插件，由
# hw_get_module() 在运行时发现。
add_subdirectory(libhardware)
if(NOT TARGET hardware_base)
    message(FATAL_ERROR "platform/libhardware must define target 'hardware_base'")
endif()
add_library(DarkOS::HardwareBase ALIAS hardware_base)

add_subdirectory(interfaces)
add_library(darkos_platform INTERFACE)
target_link_libraries(darkos_platform INTERFACE hardware_interfaces)
add_library(DarkOS::Platform ALIAS darkos_platform)

if(DARKOS_BUILD_HAL_IMPLEMENTATIONS)
    if(DARKOS_VENDOR_RESOLVED)
        set(implementation_dir
            "${CMAKE_CURRENT_SOURCE_DIR}/vendors/${DARKOS_VENDOR_RESOLVED}")
        set(hal_target
            "hal_${DARKOS_VENDOR_RESOLVED}_${DARKOS_SOC_RESOLVED}")
        set(implementation_name
            "${DARKOS_VENDOR_RESOLVED}/${DARKOS_SOC_RESOLVED}")
    elseif(DARKOS_PLATFORM_RESOLVED)
        set(implementation_dir
            "${CMAKE_CURRENT_SOURCE_DIR}/${DARKOS_PLATFORM_RESOLVED}")
        if(DARKOS_PLATFORM_RESOLVED STREQUAL "ubuntu_x86_64")
            set(hal_target hal_host_x86)
        else()
            set(hal_target "hal_${DARKOS_PLATFORM_RESOLVED}")
        endif()
        set(implementation_name "${DARKOS_PLATFORM_RESOLVED}")
    else()
        message(FATAL_ERROR
            "Cannot detect a HAL implementation for "
            "CMAKE_SYSTEM_NAME='${CMAKE_SYSTEM_NAME}', "
            "CMAKE_SYSTEM_PROCESSOR='${CMAKE_SYSTEM_PROCESSOR}'.\n"
            "Set DARKOS_PLATFORM for a reference target, or set both "
            "DARKOS_VENDOR and DARKOS_SOC for a hardware target.")
    endif()

    if(NOT EXISTS "${implementation_dir}/CMakeLists.txt")
        message(FATAL_ERROR
            "HAL implementation '${implementation_name}' is registered but "
            "not implemented: ${implementation_dir}")
    endif()
    add_subdirectory("${implementation_dir}")
    if(NOT TARGET "${hal_target}")
        message(FATAL_ERROR
            "HAL implementation '${implementation_name}' must define target "
            "'${hal_target}'")
    endif()

    # 构建插件但不写入 Application 的 DT_NEEDED；应用仍通过 dlopen 加载。
    add_dependencies(darkos_platform "${hal_target}")
    message(STATUS
        "DarkOS HAL implementation: ${implementation_name} "
        "(variant ${DARKOS_HAL_DEFAULT_VARIANT})")
elseif(DARKOS_VENDOR_RESOLVED)
    message(STATUS
        "DarkOS HAL target: ${DARKOS_VENDOR_RESOLVED}/${DARKOS_SOC_RESOLVED} "
        "(interfaces only)")
elseif(DARKOS_PLATFORM_RESOLVED)
    message(STATUS
        "DarkOS platform: ${DARKOS_PLATFORM_RESOLVED} (interfaces only)")
else()
    message(STATUS "DarkOS platform: interfaces only")
endif()

if(DARKOS_BUILD_EXAMPLES AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()

if(DARKOS_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(tests)
endif()
