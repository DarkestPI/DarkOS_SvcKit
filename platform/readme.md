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
- `allwinner`：全志
- `artosyn`：库芯微
- `ingenic`：北京君正
- `novatek`：联咏
- `sunplus`：新唐

当前仓库包含 `ubuntu_x86_64` 和 `rockchip` 的实现目录。具体 HAL 实现还依赖
`platform/libhardware` 加载器；该目录缺失时，CMake 会在开启实现选项时给出明确错误。
