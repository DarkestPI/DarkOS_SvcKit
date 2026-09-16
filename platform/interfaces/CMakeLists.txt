# interfaces：一接口一包，仿 Android hardware/interfaces 的组织方式。
# 各包全是"INTERFACE 库 + include 路径 + 依赖 hardware_base"的样板，
# 用循环统一生成；某包将来需要特殊设置时在循环外单独补即可。
set(HAL_INTERFACE_PACKAGES
    camera      # 视频输入 VI/ISP
    codec       # 编解码
    graphics    # 2D 加速/合成
    display     # 显示输出（VO，本地预览/回放）
    audio       # 音频采集/播放（语音对讲）
    wifi        # 无线网络（wpa_supplicant/hostapd）
    bluetooth   # 蓝牙（BlueZ，BLE 配网）
    sensors     # 传感器（光敏/温度/加速度等）
    gnss        # 定位（GPS/北斗等）
    light       # 指示灯/红外/白光补光（GPIO/PWM）
    serial      # 串口（UART，termios 实现位于 hardware/shared，透出 fd）
)

foreach(pkg ${HAL_INTERFACE_PACKAGES})
    add_library(${pkg}_interfaces INTERFACE)
    # include 根指向 interfaces/，使 <camera/xxx.h> 能按包名解析
    target_include_directories(${pkg}_interfaces INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${pkg}_interfaces INTERFACE hardware_base)
    add_library(DarkOS::${pkg} ALIAS ${pkg}_interfaces)
endforeach()

# 对外使用语义化的 PascalCase 名称；小写别名暂时保留给其他接口包的既有风格。
add_library(DarkOS::Codec ALIAS codec_interfaces)

# 聚合目标：一次链接全部接口包（用法：target_link_libraries(x PRIVATE hardware_interfaces)）。
# 各包仍是 INTERFACE 库，不产生任何编译产物；需要单包精细依赖时仍可单独链。
# （HAL_INTERFACE_PACKAGES 里是包名 camera/media/...，目标名是 <包名>_interfaces，故逐个转换）
add_library(hardware_interfaces INTERFACE)
foreach(pkg ${HAL_INTERFACE_PACKAGES})
    target_link_libraries(hardware_interfaces INTERFACE ${pkg}_interfaces)
endforeach()

add_library(DarkOS::HardwareInterfaces ALIAS hardware_interfaces)
