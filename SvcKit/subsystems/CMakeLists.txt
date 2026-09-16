# 骨架阶段先建立稳定的组件目标，应用和协议层只依赖目标名。
add_library(svckit_stream INTERFACE)
target_link_libraries(svckit_stream INTERFACE svckit_base DarkOS::Platform)

add_library(svckit_control_events INTERFACE)
target_link_libraries(svckit_control_events INTERFACE svckit_base)

add_library(svckit_control INTERFACE)
target_link_libraries(svckit_control INTERFACE svckit_base svckit_control_events)

add_library(svckit_peripheral INTERFACE)
target_link_libraries(svckit_peripheral INTERFACE svckit_base DarkOS::Platform)

foreach(component analytics audio networking)
    add_library(svckit_${component} INTERFACE)
    target_link_libraries(svckit_${component} INTERFACE svckit_base)
endforeach()

# Media 是对上层暴露的真实媒体框架，内部通过 Platform Camera/Codec SPI
# 访问硬件；Application 和协议层不应再自行拼接 HAL 设备。
add_subdirectory(media)

target_link_libraries(svckit_media INTERFACE svckit_stream)
target_link_libraries(svckit_audio INTERFACE DarkOS::audio)
target_link_libraries(svckit_networking INTERFACE svckit_control)

add_library(DarkOS::Stream ALIAS svckit_stream)
add_library(DarkOS::Control ALIAS svckit_control)
add_library(DarkOS::Peripheral ALIAS svckit_peripheral)
add_library(DarkOS::Analytics ALIAS svckit_analytics)
add_library(DarkOS::Audio ALIAS svckit_audio)
add_library(DarkOS::Media ALIAS svckit_media)
add_library(DarkOS::Networking ALIAS svckit_networking)
