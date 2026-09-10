# rockchip：厂商实现（RV1126B，板子用）
#
# 合并库 hal.rockchip.so（无 lib 前缀），每模块导出 HMI_<模块id>：
#   common/mpi_sys_guard.c    -> 进程级 SYS_Init/Exit 引用计数（模块间共享）
#   common/rockit_log.c       -> Rockit 启动/运行时日志等级控制
#   camera/camera_rockchip.c  -> HMI_camera（rockit MPI VI 采集 + rkaiq 3A，多实例）
#   media/codec_rockchip.c    -> HMI_codec（rockit MPI VENC/VDEC H.264 硬编解，多实例）
#   audio/audio_rockchip.c    -> HMI_audio（rockit MPI AI/AO，PCM S16LE）
#   display/display_rockchip.c -> HMI_display（rockit MPI VO 显示输出）
#   light/light_rockchip.c    -> HMI_light（sysfs GPIO，无 MPI 依赖，始终编译）
#   graphics/graphics_rockchip.c -> HMI_graphics（rockit MPI RGN OSD overlay）
#   ../shared/serial/serial_termios.c -> HMI_serial（跨厂商共享，始终编译）
# 依赖 RV1126B SDK 媒体库（DARKOS_RV1126B_MEDIA_ROOT，由
# arm-rockchip1240-gnueabihf.cmake 工具链文件给出）；未定义时只编 camera/light
# 且不链接 rockit（旧 RV1126B 工具链不可用本实现，仅为占位）。
find_package(Threads REQUIRED)

set(HAL_ROCKCHIP_SRCS
    camera/camera_rockchip.c
    light/light_rockchip.c
    common/rockit_log.c
    ../shared/serial/serial_termios.c) # termios，与 host_x86 共用同一源文件
set(HAL_ROCKCHIP_LIBS camera_interfaces light_interfaces serial_interfaces Threads::Threads)

if(DEFINED DARKOS_RV1126B_MEDIA_ROOT)
    list(APPEND HAL_ROCKCHIP_SRCS
        common/mpi_sys_guard.c
        media/codec_rockchip.c
        audio/audio_rockchip.c
        display/display_rockchip.c
        graphics/graphics_rockchip.c) # RGN 在 librockit 内，不引新库
    list(APPEND HAL_ROCKCHIP_LIBS media_interfaces audio_interfaces display_interfaces
        graphics_interfaces
        ${DARKOS_RV1126B_MEDIA_ROOT}/lib/librockit.so # VI/VENC/VDEC/AI/AO/VO/RGN 全走 rockit
        ${DARKOS_RV1126B_MEDIA_ROOT}/lib/librkaiq.so) # camera 3A（ISP 调参）
endif()

add_library(hal_rockchip SHARED ${HAL_ROCKCHIP_SRCS})
darkos_prepare_target_output(hal_rockchip)

# "common/mpi_sys_guard.h" 等以 rockchip/ 为根的相对引用
target_include_directories(hal_rockchip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(DEFINED DARKOS_RV1126B_MEDIA_ROOT)
    target_include_directories(hal_rockchip PRIVATE ${DARKOS_RV1126B_MEDIA_ROOT}/include
        ${DARKOS_RV1126B_MEDIA_ROOT}/include/rkaiq) # rkaiq 内部以 xcore/... 相对引用
endif()

target_link_libraries(hal_rockchip PRIVATE ${HAL_ROCKCHIP_LIBS})

set_target_properties(hal_rockchip PROPERTIES
    OUTPUT_NAME "hal.rockchip"
    PREFIX ""
)
