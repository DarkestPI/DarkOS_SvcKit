set(DARKOS_VISINEXTEK_SUPPORTED_SOCS vs816)
if(NOT DARKOS_SOC_RESOLVED IN_LIST DARKOS_VISINEXTEK_SUPPORTED_SOCS)
    message(FATAL_ERROR
        "Unsupported Visinextek SoC '${DARKOS_SOC_RESOLVED}'. Implemented: ${DARKOS_VISINEXTEK_SUPPORTED_SOCS}")
endif()

if(NOT DEFINED DARKOS_VS816_MEDIA_ROOT OR
   NOT EXISTS "${DARKOS_VS816_MEDIA_ROOT}/include/vs_mal_sys.h")
    message(FATAL_ERROR "DARKOS_VS816_MEDIA_ROOT must point at the SDK vs-mp directory")
endif()
if(NOT DEFINED DARKOS_VS816_SAMPLE_ROOT OR
   NOT EXISTS "${DARKOS_VS816_SAMPLE_ROOT}/common/sample_common_vii.c")
    message(FATAL_ERROR "DARKOS_VS816_SAMPLE_ROOT must point at build/vs-sample")
endif()

find_package(Threads REQUIRED)
set(DARKOS_VS816_SENSOR0_TYPE "SONY_IMX334_MIPI_8M_30FPS_12BIT" CACHE STRING
    "SDK sample sensor enum used for VS816 camera0")
set(DARKOS_VS816_SENSOR1_TYPE "OV_OS04A10_MIPI_4M_30FPS_10BIT" CACHE STRING
    "SDK sample sensor enum used for VS816 camera1")

set(vs816_target hal_visinextek_vs816)
set(vs816_sources
    common/vs_sys_guard.c
    common/vs_venc_stream.c
    socs/vs816/camera/camera_vs816.c
    socs/vs816/codec/codec_vs816.c
    socs/vs816/light/light_vs816.c
    ../../shared/linux/gpio/gpio_sysfs.c
    ../../shared/linux/serial/serial_termios.c
    "${DARKOS_VS816_SAMPLE_ROOT}/common/sample_common_vii.c")

set(vs816_native_sources
    common/vs_sys_guard.c
    common/vs_venc_stream.c
    socs/vs816/camera/camera_vs816.c
    socs/vs816/codec/codec_vs816.c
    socs/vs816/light/light_vs816.c)
set_source_files_properties(${vs816_native_sources} PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Werror")

add_library(${vs816_target} SHARED ${vs816_sources})
darkos_prepare_target_output(${vs816_target})
target_compile_definitions(${vs816_target} PRIVATE
    _DEFAULT_SOURCE VS_ORION VS_815 VS_816
    SENSOR0_TYPE=${DARKOS_VS816_SENSOR0_TYPE}
    SENSOR1_TYPE=${DARKOS_VS816_SENSOR1_TYPE}
    SENSOR2_TYPE=OV_OS04A10_MIPI_4M_30FPS_10BIT
    SENSOR3_TYPE=OV_OS04A10_MIPI_4M_30FPS_10BIT)
target_compile_options(${vs816_target} PRIVATE -include stdint.h)
target_link_options(${vs816_target} PRIVATE "-Wl,-z,defs")
target_include_directories(${vs816_target} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../shared/linux/gpio)
target_include_directories(${vs816_target} SYSTEM PRIVATE
    "${DARKOS_VS816_MEDIA_ROOT}/include"
    "${DARKOS_VS816_SAMPLE_ROOT}/common")

set(vs816_lib "${DARKOS_VS816_MEDIA_ROOT}/lib")
set(vs816_link_names
    base sys init vii isp mipirx vpp gdc venc vdec gpe tde rgn vo
    ae awb GAL)
set(vs816_link_libraries)
foreach(lib IN LISTS vs816_link_names)
    if(lib STREQUAL "GAL")
        set(path "${vs816_lib}/nn/libGAL.so")
    else()
        set(path "${vs816_lib}/lib${lib}.so")
    endif()
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "VS816 SDK library not found: ${path}")
    endif()
    list(APPEND vs816_link_libraries "${path}")
endforeach()

target_link_libraries(${vs816_target} PRIVATE
    camera_interfaces codec_interfaces light_interfaces serial_interfaces
    Threads::Threads ${CMAKE_DL_LIBS} m ${vs816_link_libraries})
set_target_properties(${vs816_target} PROPERTIES
    OUTPUT_NAME "hal.visinextek.vs816"
    PREFIX ""
    BUILD_RPATH "$ORIGIN:$ORIGIN/../.."
    INSTALL_RPATH "$ORIGIN:$ORIGIN/../.."
    BUILD_WITH_INSTALL_RPATH TRUE)

set(vs816_runtime_names
    base sys init vii isp mipirx vpp gdc venc vdec gpe tde rgn vo ae awb)
foreach(lib IN LISTS vs816_runtime_names)
    set(src "${vs816_lib}/lib${lib}.so")
    if(DARKOS_ENABLE_OUTPUT_LAYOUT)
        add_custom_command(TARGET ${vs816_target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${src}" "${DARKOS_OUTPUT_ROOT}/lib/lib${lib}.so")
    endif()
    install(FILES "${src}" DESTINATION lib)
endforeach()
foreach(nnlib libGAL.so libVIPlite.so libVIPuser.so)
    set(src "${vs816_lib}/nn/${nnlib}")
    if(DARKOS_ENABLE_OUTPUT_LAYOUT)
        add_custom_command(TARGET ${vs816_target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${src}" "${DARKOS_OUTPUT_ROOT}/lib/${nnlib}")
    endif()
    install(FILES "${src}" DESTINATION lib)
endforeach()
file(GLOB vs816_sensor_libraries "${vs816_lib}/libsnsr*.so")
foreach(src IN LISTS vs816_sensor_libraries)
    get_filename_component(name "${src}" NAME)
    if(DARKOS_ENABLE_OUTPUT_LAYOUT)
        add_custom_command(TARGET ${vs816_target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${src}" "${DARKOS_OUTPUT_ROOT}/lib/${name}")
    endif()
    install(FILES "${src}" DESTINATION lib)
endforeach()
install(TARGETS ${vs816_target} LIBRARY DESTINATION lib/darkos/hal)
