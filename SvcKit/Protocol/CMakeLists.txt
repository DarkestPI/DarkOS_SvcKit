# 协议适配层：每个协议一个子目录（信令 + 打包接线，不碰帧源）
add_subdirectory(rtsp)
add_subdirectory(gb28181)
add_subdirectory(onvif)
add_subdirectory(rtmp)
add_subdirectory(uart)
add_subdirectory(web)

add_library(darkos_protocols INTERFACE)
target_link_libraries(darkos_protocols INTERFACE
    darkos_rtsp
    darkos_gb28181
    darkos_onvif
    darkos_rtmp
    darkos_uart
    darkos_web
)
add_library(DarkOS::Protocols ALIAS darkos_protocols)
