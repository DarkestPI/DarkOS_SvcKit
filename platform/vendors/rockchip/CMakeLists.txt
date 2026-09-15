# Rockchip 厂商入口。这里只做 SoC 分发和真正跨芯片的公共依赖组装；具体媒体、
# 音频和显示实现必须归属到 socs/<soc>，避免把某一颗芯片伪装成 Rockchip 通用 HAL。
set(DARKOS_ROCKCHIP_SUPPORTED_SOCS rv1126b)

if(NOT DARKOS_SOC_RESOLVED IN_LIST DARKOS_ROCKCHIP_SUPPORTED_SOCS)
    message(FATAL_ERROR
        "Unsupported Rockchip SoC '${DARKOS_SOC_RESOLVED}'.\n"
        "Implemented Rockchip SoCs: ${DARKOS_ROCKCHIP_SUPPORTED_SOCS}")
endif()

find_package(Threads REQUIRED)

set(rockchip_soc_root
    "${CMAKE_CURRENT_SOURCE_DIR}/socs/${DARKOS_SOC_RESOLVED}")
set(rockchip_hal_target "hal_rockchip_${DARKOS_SOC_RESOLVED}")

if(DARKOS_SOC_RESOLVED STREQUAL "rv1126b")
    set(rockchip_hal_sources
        ${rockchip_soc_root}/light/light_rockchip.c
        common/rockit_log.c
        ../../shared/linux/gpio/gpio_sysfs.c
        ../../shared/linux/serial/serial_termios.c)
    set(rockchip_hal_libraries
        light_interfaces
        serial_interfaces
        Threads::Threads)

    # 媒体模块依赖 RV1126B Rockit/RKAIQ SDK。没有媒体 SDK 时仍可构建 GPIO 和
    # termios 模块，便于先验证 HAL 装载及基础外设。
    if(DEFINED DARKOS_RV1126B_MEDIA_ROOT)
        list(APPEND rockchip_hal_sources
            ${rockchip_soc_root}/camera/camera_rockchip.c
            common/mpi_sys_guard.c
            ${rockchip_soc_root}/media/codec_rockchip.c
            ${rockchip_soc_root}/audio/audio_rockchip.c
            ${rockchip_soc_root}/display/display_rockchip.c
            ${rockchip_soc_root}/graphics/graphics_rockchip.c)
        list(APPEND rockchip_hal_libraries
            camera_interfaces
            media_interfaces
            audio_interfaces
            display_interfaces
            graphics_interfaces
            ${DARKOS_RV1126B_MEDIA_ROOT}/lib/librockit.so
            ${DARKOS_RV1126B_MEDIA_ROOT}/lib/librkaiq.so)
    endif()
endif()

add_library(${rockchip_hal_target} SHARED ${rockchip_hal_sources})
darkos_prepare_target_output(${rockchip_hal_target})

# SoC 源码通过 common/... 引用厂商公共辅助代码。
target_include_directories(${rockchip_hal_target} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../shared/linux/gpio)

if(DEFINED DARKOS_RV1126B_MEDIA_ROOT)
    target_include_directories(${rockchip_hal_target} PRIVATE
        ${DARKOS_RV1126B_MEDIA_ROOT}/include
        ${DARKOS_RV1126B_MEDIA_ROOT}/include/rkaiq)
endif()

target_link_libraries(${rockchip_hal_target} PRIVATE ${rockchip_hal_libraries})

set_target_properties(${rockchip_hal_target} PROPERTIES
    OUTPUT_NAME "hal.rockchip.${DARKOS_SOC_RESOLVED}"
    PREFIX "")

install(TARGETS ${rockchip_hal_target} LIBRARY DESTINATION lib/darkos/hal)
