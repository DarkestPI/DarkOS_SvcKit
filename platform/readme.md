# Platform 层

Platform 是仿 Android legacy HAL 的纯 C 硬件抽象层。接口、Linux 公共机制、
厂商公共代码和具体 SoC 实现分开维护；SvcKit 只依赖接口，Application 通过
`libhardware` 在运行时加载 HAL 插件。

## 目录边界

```text
platform/
├── interfaces/                       稳定 HAL ABI
├── libhardware/                      hal.<variant>.so 加载器
├── shared/linux/                     不依赖芯片 SDK 的 Linux 公共后端
│   ├── camera/camera_v4l2.c          V4L2/UVC 采集
│   ├── gpio/gpio_sysfs.c             legacy sysfs GPIO
│   └── serial/serial_termios.c        termios/RS-485 串口
├── vendors/
│   ├── rockchip/
│   │   ├── common/                   Rockchip SoC 间可复用的 Rockit 辅助代码
│   │   └── socs/rv1126b/             RV1126B Camera/Media/Audio/Display 等实现
│   └── visinextek/
│       ├── common/                   VS SYS/VB 进程级生命周期
│       └── socs/vs816/               VS816 Camera/Codec/Light 实现
└── ubuntu_x86_64/                    Ubuntu 主机参考/模拟 HAL
```

`shared` 不导出厂商模块，也不知道 Rockchip/RV1126B。厂商层负责选择和组装公共
后端，SoC 层负责 SDK ABI、媒体通道、ISP/NPU 和硬件能力差异。设备节点、GPIO
编号、串口用途等装配信息仍属于 Board 配置。

## 构建选择

默认只构建接口：

```cmake
DARKOS_BUILD_HAL_IMPLEMENTATIONS=OFF
```

Ubuntu x86_64 参考实现：

```bash
cmake -S . -B build \
    -DDARKOS_BUILD_HAL_IMPLEMENTATIONS=ON \
    -DDARKOS_PLATFORM=ubuntu_x86_64
```

真实 Rockchip SoC 使用 Vendor + SoC 两级选择：

```bash
cmake -S . -B build \
    -DDARKOS_BUILD_HAL_IMPLEMENTATIONS=ON \
    -DDARKOS_VENDOR=rockchip \
    -DDARKOS_SOC=rv1126b
```

RV1126B 工具链变量存在时可以自动识别 vendor 和 SoC。旧的
`DARKOS_PLATFORM=rockchip` 暂时兼容，但已弃用。

VS816 使用配套发布包和工具链：

```bash
cmake -S . -B build-vs816 \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-visinextek-linux-gnu.cmake \
    -DDARKOS_BUILD_HAL_IMPLEMENTATIONS=ON \
    -DDARKOS_VENDOR=visinextek \
    -DDARKOS_SOC=vs816
cmake --build build-vs816 --target hal_visinextek_vs816 -j
```

当前已提供实现的组合是 Rockchip/RV1126B 和 Visinextek/VS816（后者当前覆盖
Camera、Codec、Light、Serial）。其他已登记 vendor 仅表示可选择的命名空间；
选择尚未实现的组合时 CMake 会明确报错。

## 插件命名与加载

- Ubuntu x86_64：`hal.host_x86.so`
- Rockchip RV1126B：`hal.rockchip.rv1126b.so`
- Visinextek VS816：`hal.visinextek.vs816.so`

Application 不直接链接插件。`hw_get_module("camera")` 会读取
`DARKOS_HAL_VARIANT`，查找 `hal.<variant>.so`，再通过 `dlsym("HMI_camera")`
获取模块。未设置环境变量时使用构建阶段选定的默认 variant。

开发阶段可以覆盖搜索目录：

```bash
DARKOS_HAL_VARIANT=rockchip.rv1126b \
DARKOS_HAL_LIBRARY_PATH=/path/to/output/lib application
```

安装后的默认搜索路径包含 `/usr/lib/darkos/hal`。插件按进程缓存，Application
只链接 `libhardware` 与公共接口，因此不会产生厂商 HAL 的 `DT_NEEDED` 依赖。
