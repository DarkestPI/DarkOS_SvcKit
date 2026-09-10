

camera_interfaces      # 视频输入 VI/ISP
media_interfaces       # 编解码
graphics_interfaces    # 2D 加速/合成
audio_interfaces       # 音频采集/播放（ALSA，语音对讲）
wifi_interfaces        # 无线网络（wpa_supplicant/hostapd）
bluetooth_interfaces   # 蓝牙（BlueZ，BLE 配网）
sensors_interfaces     # 传感器（光敏/温度/加速度等）
gnss_interfaces        # 定位（GPS/北斗等）
light_interfaces       # 指示灯/红外/白光补光（GPIO/PWM）

```bash
.
├── audio           # 音频采集/播放（ALSA，语音对讲）
├── bluetooth       # 蓝牙功能
├── camera          # 视频输入 VI/ISP
├── display         # 显示器输出
├── gnss            # 定位（GPS/北斗等）
├── graphics        # 2D 加速 合成
├── light           # 指示灯/红外/白光补光（GPIO/PWM）
├── media           # 多媒体编解码
├── sensors         # 传感器（光敏/温度/加速度等）
├── serial          # 串口
└── wifi            # WIFI 功能

```
