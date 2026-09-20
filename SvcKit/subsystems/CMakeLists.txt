# 骨架阶段先建立稳定的组件目标，应用和协议层只依赖目标名。
foreach(component analytics audio)
    add_library(svckit_${component} INTERFACE)
    target_link_libraries(svckit_${component} INTERFACE svckit_base)
endforeach()

# Media 是对上层暴露的真实媒体框架，内部通过 Platform Camera/Codec SPI
# 访问硬件；Application 和协议层不应再自行拼接 HAL 设备。
add_subdirectory(peripheral)
add_subdirectory(media)
add_subdirectory(storage)
add_subdirectory(alarm)
add_subdirectory(network)
add_subdirectory(iva)

target_link_libraries(svckit_analytics INTERFACE svckit_iva)

target_link_libraries(svckit_audio INTERFACE DarkOS::audio)

add_library(DarkOS::Peripheral ALIAS svckit_peripheral)
add_library(DarkOS::Analytics ALIAS svckit_analytics)
add_library(DarkOS::Audio ALIAS svckit_audio)
add_library(DarkOS::Media ALIAS svckit_media)
add_library(DarkOS::Networking ALIAS svckit_networking)
add_library(DarkOS::Storage ALIAS svckit_storage)
add_library(DarkOS::Alarm ALIAS svckit_alarm)
add_library(DarkOS::IVA ALIAS svckit_iva)
