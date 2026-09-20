# Rockchip 厂商入口。这里只做 SoC 分发和真正跨芯片的公共依赖组装；具体媒体、
# 音频和显示实现必须归属到 socs/<soc>，避免把某一颗芯片伪装成 Rockchip 通用 HAL。
option(DARKOS_ENABLE_RV1126B_RKNN
    "Build the RV1126B RKNN inference HAL adapter" OFF)
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
            ${rockchip_soc_root}/codec/codec_rockchip.c
            ${rockchip_soc_root}/audio/audio_rockchip.c
            ${rockchip_soc_root}/display/display_rockchip.c
            ${rockchip_soc_root}/graphics/graphics_rockchip.c)
        list(APPEND rockchip_hal_libraries
            camera_interfaces
            codec_interfaces
            audio_interfaces
            display_interfaces
            graphics_interfaces
            ${DARKOS_RV1126B_MEDIA_ROOT}/lib/librockit.so
        ${DARKOS_RV1126B_MEDIA_ROOT}/lib/librkaiq.so)
    endif()

    # 推理能力属于 Platform HAL，不属于 SvcKit IVA。SDK 存在时把 RV1126B
    # 的 RKNN 适配器编进同一个 HAL 插件，并由 HMI_inference 导出通用 SPI。
    if(DARKOS_ENABLE_RV1126B_RKNN)
        if(NOT DEFINED DARKOS_RV1126B_RKNN_ROOT)
            message(FATAL_ERROR
                "DARKOS_ENABLE_RV1126B_RKNN is ON, but "
                "DARKOS_RV1126B_RKNN_ROOT is not set")
        endif()
        set(rockchip_rknn_library
            "${DARKOS_RV1126B_RKNN_ROOT}/Linux/armhf/librknnrt.so")
        if(NOT EXISTS "${DARKOS_RV1126B_RKNN_ROOT}/include/rknn_api.h" OR
           NOT EXISTS "${rockchip_rknn_library}")
            message(FATAL_ERROR
                "DARKOS_RV1126B_RKNN_ROOT is set, but the RV1126B RKNN SDK "
                "does not contain include/rknn_api.h and Linux/armhf/librknnrt.so: "
                "${DARKOS_RV1126B_RKNN_ROOT}")
        endif()
        list(APPEND rockchip_hal_sources
            ${rockchip_soc_root}/inference/inference_rknn.cpp)
        list(APPEND rockchip_hal_libraries
            ${rockchip_rknn_library})
        set(rockchip_has_rknn TRUE)
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

# Rockit 的依赖不是 HAL 的直接链接项，动态加载 HAL 时仍然必须能在板端找到
# 这些 SONAME。统一从 SDK 的 media_out 复制到 output/lib，避免“编译成功、
# hw_get_module() 因缺少 librockit/libmpp/libdrm/libasound 而加载失败”。
if(DEFINED DARKOS_RV1126B_MEDIA_ROOT)
    set(rockchip_runtime_files
        "${DARKOS_RV1126B_MEDIA_ROOT}/lib/librockit.so|librockit.so"
        "${DARKOS_RV1126B_MEDIA_ROOT}/lib/librkaiq.so|librkaiq.so"
        "${DARKOS_RV1126B_MEDIA_ROOT}/lib/librockchip_mpp.so.0|librockchip_mpp.so.0"
        "${DARKOS_RV1126B_MEDIA_ROOT}/lib/librockchip_mpp.so.0|librockchip_mpp.so.1"
        "${DARKOS_RV1126B_MEDIA_ROOT}/lib/librockchip_mpp.so.0|librockchip_mpp.so"
        "${DARKOS_RV1126B_MEDIA_ROOT}/lib/libdrm.so.2.4.0|libdrm.so.2.4.0"
        "${DARKOS_RV1126B_MEDIA_ROOT}/lib/libdrm.so.2.4.0|libdrm.so.2"
        "${DARKOS_RV1126B_MEDIA_ROOT}/root/usr/lib/libasound.so.2.0.0|libasound.so.2.0.0"
        "${DARKOS_RV1126B_MEDIA_ROOT}/root/usr/lib/libasound.so.2.0.0|libasound.so.2")

    foreach(runtime_spec IN LISTS rockchip_runtime_files)
        string(REPLACE "|" ";" runtime_parts "${runtime_spec}")
        list(GET runtime_parts 0 runtime_source)
        list(GET runtime_parts 1 runtime_name)
        if(EXISTS "${runtime_source}")
            if(DARKOS_ENABLE_OUTPUT_LAYOUT)
                add_custom_command(TARGET ${rockchip_hal_target} POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E make_directory
                        "${DARKOS_OUTPUT_ROOT}/lib"
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${runtime_source}"
                        "${DARKOS_OUTPUT_ROOT}/lib/${runtime_name}"
                    COMMENT "Staging Rockchip runtime ${runtime_name}")
            endif()
            install(FILES "${runtime_source}" DESTINATION lib
                RENAME "${runtime_name}")
        else()
            message(WARNING
                "Rockchip runtime '${runtime_name}' was not found at "
                "${runtime_source}; board-side HAL loading may fail")
        endif()
    endforeach()
endif()

if(rockchip_has_rknn)
    target_include_directories(${rockchip_hal_target} PRIVATE
        ${DARKOS_RV1126B_RKNN_ROOT}/include)
    target_compile_features(${rockchip_hal_target} PRIVATE cxx_std_17)
    if(DARKOS_ENABLE_OUTPUT_LAYOUT)
        add_custom_command(TARGET ${rockchip_hal_target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${rockchip_rknn_library}"
                "${DARKOS_OUTPUT_ROOT}/lib/librknnrt.so"
            COMMENT "Staging RV1126B RKNN Runtime")
    endif()
    install(FILES "${rockchip_rknn_library}" DESTINATION lib
        RENAME librknnrt.so)
    message(STATUS "Rockchip RV1126B inference HAL: RKNN")
endif()

set_target_properties(${rockchip_hal_target} PROPERTIES
    OUTPUT_NAME "hal.rockchip.${DARKOS_SOC_RESOLVED}"
    PREFIX "")

if(rockchip_has_rknn AND DARKOS_ENABLE_OUTPUT_LAYOUT)
    # HAL 位于 lib/darkos/hal，RKNN Runtime 位于 lib；保留相对运行时搜索路径，
    # 这样通过 hw_get_module() dlopen HAL 时不依赖开发机 SDK 的绝对路径。
    set_target_properties(${rockchip_hal_target} PROPERTIES
        BUILD_RPATH "$ORIGIN:$ORIGIN/../.."
        INSTALL_RPATH "$ORIGIN:$ORIGIN/../.."
        BUILD_WITH_INSTALL_RPATH TRUE)
endif()

install(TARGETS ${rockchip_hal_target} LIBRARY DESTINATION lib/darkos/hal)
