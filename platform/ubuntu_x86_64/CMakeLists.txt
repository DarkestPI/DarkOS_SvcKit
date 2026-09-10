# host_x86：主机参考实现（无硬件依赖，供宿主机开发/联调）
#
# 所有模块直接编进一个合并库 hal.host_x86.so（每模块导出 HMI_<模块id>，
# 加载器 dlsym("HMI_" id) 查找，见 libhardware/hardware.c）。
# 新增模块：往源文件列表加一行即可。
find_package(Threads REQUIRED)

add_library(hal_host_x86 SHARED
    camera/camera_host_x86.c         # 合成测试图案（DARKOS_CAMERA_DEVICE 分流 UVC） -> HMI_camera
    camera/camera_uvc.c              # UVC/V4L2 真实摄像头（YUYV→NV12）
    media/codec_host_x86.c           # RAW 透传 + x264 软编 -> HMI_codec
    audio/audio_host_x86.c           # 正弦波声源/伪播放    -> HMI_audio
    wifi/wifi_host_x86.c             # 假热点扫描/连接      -> HMI_wifi
    bluetooth/bluetooth_host_x86.c   # 假设备发现          -> HMI_bluetooth
    sensors/sensors_host_x86.c       # 模拟光敏/温度        -> HMI_sensors
    gnss/gnss_host_x86.c             # 1Hz 移动定位点      -> HMI_gnss
    light/light_host_x86.c           # 内存态灯            -> HMI_light
    graphics/graphics_host_x86.c     # OSD canvas（malloc+DARKOS_OSD_DUMP 存 PPM）-> HMI_graphics
    ../shared/serial/serial_termios.c # 跨厂商共享 termios 串口 -> HMI_serial
)

darkos_prepare_target_output(hal_host_x86)

target_link_libraries(hal_host_x86 PRIVATE
    hardware_interfaces   # 全部接口包的聚合目标（见 interfaces/CMakeLists.txt）
    x264                  # codec 模块的 H.264 软编（系统 libx264）
    jpeg                  # camera UVC 模块的 MJPEG 解码（系统 libjpeg）
    Threads::Threads      # camera 等模块的采集线程
)

set_target_properties(hal_host_x86 PROPERTIES
    OUTPUT_NAME "hal.host_x86"
    PREFIX ""
)
