# Platform 层

Platform 是仿 Android HAL 的纯 C 硬件抽象层。公共接口与厂商实现分离；
SvcKit 始终依赖 `DarkOS::Platform`，具体实现由 CMake 配置选择。

## CMake 选择方式

- `DARKOS_BUILD_HAL_IMPLEMENTATIONS=OFF`（默认）：只建立公共接口目标，不编译
  具体平台实现。
- `DARKOS_BUILD_HAL_IMPLEMENTATIONS=ON`：编译 `DARKOS_PLATFORM` 指定的唯一实现。
- `DARKOS_PLATFORM=auto`（默认）：RV1126B 工具链选择 `rockchip`，本机 Linux
  x86-64 选择 `ubuntu_x86_64`。不能可靠识别时必须显式指定。

例如：

```bash
cmake -S . -B build/host \
    -DDARKOS_PLATFORM=ubuntu_x86_64 \
    -DDARKOS_BUILD_HAL_IMPLEMENTATIONS=ON
```

平台名统一在根 `CMakeLists.txt` 中登记，但只会加入当前选中的一个子目录。
这样尚未实现的平台不会导致其他目标配置失败。

## 平台名

- `ubuntu_x86_64`：Ubuntu x86-64 主机参考实现
- `rockchip`：瑞芯微
- `gokemicro`：国科微
- `allwinner`：全志
- `artosyn`：库芯微
- `ingenic`：北京君正
- `novatek`：联咏
- `sunplus`：新唐

当前仓库包含 `ubuntu_x86_64` 和 `rockchip` 的实现目录。

## 运行时加载

Application 只链接 `libhardware` 与公共接口，不直接链接厂商 HAL。开启实现构建
后，选中的 `hal.<variant>.so` 会作为独立插件构建；`hw_get_module("camera")`
在运行时执行：

```text
读取 DARKOS_HAL_VARIANT（未设置则使用构建默认值）
  → 在 DARKOS_HAL_LIBRARY_PATH（冒号分隔）中查找 hal.<variant>.so
  → dlsym("HMI_camera")
  → 校验 tag、模块 ID、HAL ABI 主版本和 open 方法
  → 缓存并返回 module
```

安装后的默认插件目录是 `/usr/lib/darkos/hal`；开发阶段可以指向构建输出：

```bash
DARKOS_HAL_VARIANT=host_x86 \
DARKOS_HAL_LIBRARY_PATH=/path/to/output/lib application
```

`platform/shared` 保存不依赖 SoC SDK 的 Linux 通用实现。目前串口由 termios
实现，可以同时创建多个设备实例；具体 `/dev/tty*` 与业务用途仍由 Board 和
Application 配置决定。
