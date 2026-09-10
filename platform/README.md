# hardware 层

仿 Android HAL（纯 C）的硬件抽象层：**接口定义与厂商实现彻底分离**，
上层通过字符串 id 在运行时加载厂商实现，编译期不依赖任何厂商代码。

## 目录结构

```
hardware/
├── libhardware/               # 基础类型 + 加载器
│   ├── include/hardware/hardware.h  # hw_module_t/hw_device_t + 加载器 API
│   └── hardware.c             # hw_get_module（dlopen + dlsym("HMI")）
├── interfaces/                # 一接口一包，仿 Android hardware/interfaces
│   ├── camera/                #   视频输入 VI/ISP（对应 Android camera/）
│   │   ├── types.h            #     共享类型（对应 types.hal）
│   │   └── ICameraDevice.h    #     接口定义（对应 ICameraDevice.hal）
│   ├── media/                 #   编解码（对应 Android media/omx）
│   │   ├── types.h
│   │   └── ICodec.h
│   ├── graphics/              #   2D 加速/合成（对应 Android graphics/）
│   │   ├── types.h
│   │   └── IGraphics.h
│   ├── audio/                 #   音频采集/播放（语音对讲）
│   │   ├── types.h
│   │   └── IAudio.h
│   ├── display/               #   显示输出（VO，本地预览/回放）
│   │   ├── types.h
│   │   └── IDisplay.h
│   ├── wifi/                  #   无线网络（wpa_supplicant/hostapd）
│   │   ├── types.h
│   │   └── IWifi.h
│   ├── bluetooth/             #   蓝牙（BlueZ，BLE 配网）
│   │   ├── types.h
│   │   └── IBluetooth.h
│   ├── sensors/               #   传感器（光敏/温度/加速度等）
│   │   ├── types.h
│   │   └── ISensors.h
│   ├── gnss/                  #   定位（GPS/北斗等）
│   │   ├── types.h
│   │   └── IGnss.h
│   └── light/                 #   指示灯/红外/白光补光（GPIO/PWM）
│       ├── types.h
│       └── ILight.h
├── rockchip/                  # 厂商实现（RV1126B，板子用）→ 合并库 hal.rockchip.so
│   ├── common/                #   MPI SYS 引用计数、Rockit 日志等厂商内部公共设施
│   ├── camera/                #   HMI_camera：rockit MPI VI 采集 + rkaiq 3A（多实例）
│   ├── media/                 #   HMI_codec：MPI VENC/VDEC H.264 硬编解（MB 零拷贝输入，多实例）
│   ├── audio/                 #   HMI_audio：MPI AI/AO（PCM S16LE）
│   ├── display/               #   HMI_display：MPI VO 显示输出
│   └── light/                 #   HMI_light：sysfs GPIO 补光/状态灯
├── host_x86/                  # 主机参考实现（无硬件，仅本机构建）
│   │                          #   各模块源文件直编 → hal.host_x86.so
│   ├── camera/                #   合成测试图案；可分流 UVC（导出 HMI_camera）
│   ├── media/                 #   RAW 透传             （导出 HMI_codec）
│   ├── audio/                 #   正弦波声源/伪播放    （导出 HMI_audio）
│   ├── wifi/                  #   假热点扫描/连接      （导出 HMI_wifi）
│   ├── bluetooth/             #   假设备发现           （导出 HMI_bluetooth）
│   ├── sensors/               #   模拟光敏/温度        （导出 HMI_sensors）
│   ├── gnss/                  #   1Hz 移动定位点       （导出 HMI_gnss）
│   └── light/                 #   内存态灯             （导出 HMI_light）
├── shared/                    # 跨厂商复用的 HAL 实现（不是新的抽象层）
│   └── serial/                #   termios 串口，编入 rockchip/host_x86 合并库
└── examples/                  # 验证程序 hal_probe / media_probe / misc_probe
```

## 与 Android 的对应关系

| 本工程 | Android hardware/interfaces | 底层实现 |
|---|---|---|
| `interfaces/camera/` | `camera/` | rockit MPI VI + rkaiq（3A） |
| `interfaces/media/` | `media/omx` | rockit MPI VENC/VDEC 硬编解 |
| `interfaces/graphics/` | `graphics/` | librga（im2d） |
| `interfaces/display/` | `hwcomposer`（极简形态） | rockit MPI VO |
| `interfaces/audio/` | `audio/` | rockit MPI AI/AO |

> 命名说明：Android 没有 `video` / `codec` 顶层包，视频输入在 `camera/`、
> 编解码在 `media/`。运行时模块 id（`hw_get_module` 的入参）则用更直白的
> `"camera"` / `"codec"` / `"graphics"` 字符串。

## 核心机制

1. **vtable（函数指针表）**：`hw_device_t` 基类 + `ops` 操作表 + `priv` 私有
   上下文，用 C 表达"接口/实现"分离。
2. **动态加载**：厂商实现编译成**合并库** `hal.<vendor>.so`（一个变体一个文件），
   每个模块导出符号 `HMI_<模块id>`（如 `HMI_camera`、`HMI_codec`）；
   加载器用 `dlopen` + `dlsym("HMI_" id)` 找到实现。兼容旧形态：
   找不到合并库时退回 `<id>.<vendor>.so` + `dlsym("HMI")`。
3. **ABI 稳定**：`tag` 魔数 + `version` + `reserved` 预留数组。
4. **类型与接口分离**：每个包拆成 `types.h`（结构体/枚举）与 `I<接口>.h`
   （ops 方法表）。

## 命名约定

- 合并库：`hal.<vendor>.so`（如 `hal.host_x86.so`），**不带 `lib` 前缀**
  （加载器按精确名查找）；库内每个模块导出 `HMI_<模块id>` 符号。
- 现有变体：`rockchip`（厂商，板子用）、`host_x86`（主机参考，宿主机开发/联调）。
- 兼容旧形态：`<module_id>.<vendor>.so` + `HMI` 符号（rockchip 现有
  camera 模块仍在用）；加载器兜底还会尝试 `hal.default.so` /
  `<module_id>.default.so`（通用兜底实现，暂未提供）。

## 构建

所有构建都放在 `build/` 下，按目标命名子目录。

### 主机构建（host_x86，含主机参考实现）

```bash
cmake -S . -B build/host_x86
cmake --build build/host_x86
```

### 交叉编译（rv1126，ARM 32 位）

```bash
cmake -S . -B build/rv1126 \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-linux-gnueabihf.cmake
cmake --build build/rv1126
```

产物（在 `build/<目标>/oem/` 下分 `bin/` 与 `lib/`）：

- `oem/lib/libhardware.so` —— 加载器
- `oem/lib/hal.host_x86.so` —— 主机参考实现合并库（8 个模块，仅主机构建）
- `oem/lib/camera.rockchip.so` —— 厂商实现（板子用，旧独立库形态，加载器兼容）
- `oem/bin/hal_probe` —— 验证程序（camera 单链路）
- `oem/bin/media_probe` —— 验证程序（camera → codec 管道）

## 运行验证

**主机（host_x86，合成相机 + RAW 透传编码，端到端跑通）：**

```bash
DARKOS_HAL_VARIANT=host_x86 \
DARKOS_HAL_LIBRARY_PATH=$PWD/build/host_x86/oem/lib \
    ./build/host_x86/oem/bin/hal_probe
DARKOS_HAL_VARIANT=host_x86 \
DARKOS_HAL_LIBRARY_PATH=$PWD/build/host_x86/oem/lib \
    ./build/host_x86/oem/bin/media_probe   # camera → codec 管道
```

**主机接 UVC 真实摄像头（host_x86 camera 模块分流）：**

```bash
DARKOS_HAL_VARIANT=host_x86 \
DARKOS_HAL_LIBRARY_PATH=$PWD/build/host_x86/oem/lib \
DARKOS_CAMERA_DEVICE=/dev/video0 \    # 指定 V4L2 节点即走 UVC 实现（camera_uvc.c）
    ./build/host_x86/oem/bin/hal_probe
```

- UVC 采集优先 MJPEG（libjpeg 解码），`DARKOS_CAMERA_FORMAT=yuyv` 可强制 YUYV；
  无论哪种都在采集线程转换为 NV12 对上呈现，接口不变。
- 虚拟机 USB 直通常见等时传输丢包：YUYV 可能整帧不出、MJPEG 帧大量截断。
  坏帧会被丢弃/容错解码；若画面持续花屏，优先排查 VM 的 USB 控制器设置
  （切换 USB 3.x / xHCI 通常可解决）。

**板子（rockchip 真相机）：** 把 `build/rv1126/oem/lib/*.so` 拷到板子
`/vendor/lib/hw/` 后直接跑 `hal_probe`（无需设环境变量），或
`DARKOS_HAL_VARIANT=rockchip` 显式指定。

## 多实例（多路摄像头）

camera/codec 支持按实例 id 打开：`camera_open_by_id(module, "cameraN", …)`、
`codec_open_by_id(module, "codecN", …)`（裸 id `"camera"`/`"codec"` 等价实例 0）。

- rockchip：实例 N 默认映射 VI `dev=pipe=N`、`chn=0`，VENC/VDEC `chn=N`，
  rkaiq 物理相机序号 = N；用 `DARKOS_CAMERA{N}_VI_DEV/PIPE/CHN` 覆盖 VI 绑定。
- 摄像头类型（白光全彩 / 红外夜视）：`DARKOS_CAMERA{N}_TYPE=ir|white`
  （默认 white），透出到 `camera_caps_t.camera_type`。
- host_x86：合成源任意实例可用；UVC 用 `DARKOS_CAMERA_DEVICE_{N}` 指定实例 N
  的设备节点（`DARKOS_CAMERA_DEVICE` 保留，等价实例 0）。
- 上层由 MediaService 按通道数自动打开 `camera{i}`/`codec{i}`（i 为通道号）。

## 补光/状态灯（light）

- host_x86：内存态模拟（TIMED 只记录状态）。
- rockchip：sysfs GPIO。引脚是板级设计，用 env 配置（内核 GPIO 编号）：
  `DARKOS_LIGHT_IR_PIN`（红外补光）、`DARKOS_LIGHT_WHITE_PIN`（白光补光）、
  `DARKOS_LIGHT_STATUS_PIN`（状态灯）；未配置引脚的灯不进 caps。
  低电平点亮的板子加 `DARKOS_LIGHT_<X>_ACTIVE_LOW=1`。
  GPIO 无 PWM：`brightness>0` 即亮；TIMED 闪烁只记录目标状态（待需要时加定时线程）。

## Rockit 日志控制

`rockchip/common/rockit_log.h` 是 `hal.rockchip.so` 内部使用的厂商辅助接口：

```c
#include "common/rockit_log.h"

/* 必须在首次调用 Rockit API 前设置，等价于 export rt_log_level=3。 */
rockit_log_set_startup_level(3);

/* Rockit 已运行时动态设置，等价于 echo "venc=6" > /tmp/rt_log_level。 */
rockit_log_set_module_level("venc", 6);
```

首次执行 `RK_MPI_SYS_Init()` 前会自动设置默认等级 `3`；如果部署环境已经设置
`rt_log_level`，则以环境变量为准，代码不会覆盖。

动态设置支持 `all/cmpi/mb/sys/vdec/venc/rgn/vpss/vgs/tde/avs/wbc/vo/vi/ai/ao/aenc/adec`，
等级范围为 0..6；非法模块或等级返回 `-EINVAL`。这个接口不穿透 HAL 边界，未来若需
由 Web 修改，应先定义通用的诊断/日志控制接口，再由 Rockchip 实现翻译到这里。


## 加载器查找顺序

variant 依次尝试：

1. 环境变量 `DARKOS_HAL_VARIANT`
2. 编译期默认值 `rockchip`（见 `libhardware/hardware.c`）
3. `default`

每个 variant 内先搜 `DARKOS_HAL_LIBRARY_PATH`（冒号分隔），再搜内置路径
（`/vendor/lib/hw`、`/system/lib/hw` 等）。

## 如何新增一个接口

1. 在 `interfaces/<包>/` 下建 `types.h` + `I<接口>.h`，并把包名加进
   `interfaces/CMakeLists.txt` 的 `HAL_INTERFACE_PACKAGES` 列表；
2. 在厂商目录实现，导出 `HMI_<模块id>`：host_x86 把源文件加进
   `host_x86/CMakeLists.txt` 的源文件列表（自动进 `hal.host_x86.so`）；
   rockchip 亦可聚合成 `hal.rockchip.so`
   （或沿用旧独立库形态 `<module_id>.rockchip.so` + `HMI`，加载器兼容）；
3. 上层 `hw_get_module("<module_id>")` 即可拿到实现。

## 后续计划

- media 的 rockchip 实现：补 H.265（H.264 编/解已落地，MPI VENC/VDEC）；
- graphics 接入 librga，并补 host_x86 主机参考实现；
- 音频压缩（G.711 对讲码流）：HAL 只出 PCM，需要时在 SvcKit 服务层接
  MPI AENC/ADEC（参考 SDK rkipc：AI --bind--> AENC / ADEC --bind--> AO）；
- 帧传递已透出 dma-buf fd / MB_BLK（进程内零拷贝：camera VI → codec VENC
  直送）；VDEC → VO 上屏零拷贝（bind 模式）留待本地回放需求落地；
  跨进程零拷贝留待有跨进程需求时再做。
