# Shared Platform Backends

这里存放能够跨 CPU 厂商复用、且不依赖某个 SoC SDK 的实现。当前公共能力包括：

- `linux/serial`：termios 串口与 Linux RS-485 ioctl；
- `linux/camera`：V4L2/UVC 采集及 YUYV/MJPEG 到 NV12 转换；
- `linux/gpio`：兼容旧 vendor kernel 的 sysfs GPIO 输出辅助层。

公共后端原则上不自行决定板级设备节点、GPIO 编号或业务用途，也不负责选择 SoC。
ALSA、DRM/KMS、libgpiod、网络等能力应在出现实际调用方时继续沉淀到这里。
