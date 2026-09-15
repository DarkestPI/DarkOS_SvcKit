# interfaces — HAL 接口包

一接口一包，仿 Android hardware/interfaces 的组织方式。各包是纯头文件的
INTERFACE 库（`DarkOS::<pkg>` / 聚合 `hardware_interfaces`），实现侧
（`ubuntu_x86_64`、`vendors/*`）与调用侧（SvcKit）都只依赖这里的定义。

```text
├── audio           # 音频采集/播放（ALSA，语音对讲）
├── bluetooth       # 蓝牙功能
├── camera          # 视频输入 VI/ISP
├── display         # 显示器输出
├── gnss            # 定位（GPS/北斗等）
├── graphics        # 2D 加速 合成
├── light           # 指示灯/红外/白光补光（GPIO/PWM）
├── media           # 多媒体编解码（模块 id "codec"）
├── sensors         # 传感器（光敏/温度/加速度等）
├── serial          # 串口（termios 实现位于 shared/linux，透出 fd）
└── wifi            # WIFI 功能
```

## 接口版本契约

每个域接口带两个版本号，都按 major.minor 编码
（`HARDWARE_MAKE_API_VERSION`）：

- `*_MODULE_API_VERSION`：模块级接口版本。实现写进
  `hw_module_t.module_api_version`；
- `*_DEVICE_API_VERSION`：设备/ops 表版本。实现 open 时写进
  `hw_device_t.version`（只用低 16 位）。

校验方（不要绕过 `*_open()` 直接调 `methods->open`）：

- `hw_get_module()`（libhardware）只校验框架级 `hal_api_version` 主版本；
- 各域 `*_open()` 内联助手校验 module 与 device 版本，不满足返回
  `-EPROTONOSUPPORT`。

兼容规则：

- **主版本一致**且实现的 **minor ≥ 调用方 minor** 才兼容；
- minor 递增只许**追加**：ops 表尾部加函数指针、结构体尾部加字段，已有
  成员的位置和语义永不改变；调用方使用高 minor 新增 ops 前必须判 NULL；
- 主版本变化 = ABI 破坏，所有实现与调用方都要迁移，当成大版本工程立项。

改动接口头文件时的检查单：

1. 加"参数型"能力优先用 `set_control/get_control` 新控制项 id，不动 ops、
   不升版本；
2. 加 ops 函数/结构体字段 → minor +1，头文件新增 `*_..._VERSION_1_x` 宏，
   实现按自己实现到的版本声明，未实现新成员的保持旧版本号即可；
3. 改成员语义/顺序/删除 → major +1，通知全部平台迁移。

`hw_module_t` / `hw_device_t` 框架结构本身由 `hal_api_version` 管
（loader 校验），预计长期停留在 1.0，不要动这两个结构体。
