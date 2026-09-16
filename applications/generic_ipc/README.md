# Generic IPC（Ubuntu x86_64）

这是用于 Ubuntu x86_64 宿主机开发和验证的通用 IPC 工程。它使用本机编译器，
并构建主机参考 HAL 插件 `hal.host_x86.so`，不依赖 Rockchip 交叉工具链。

## 构建

```bash
cmake --preset generic_ipc
cmake --build --preset generic_ipc --parallel
```

输出目录：

```text
output/generic_ipc/
├── bin/generic_ipc
├── etc/app.json
├── etc/board.json
└── lib/hal.host_x86.so
```

## 运行

程序默认从可执行文件同级输出树的 `etc/` 读取配置，因此可以从任意工作目录直接运行：

```bash
./output/generic_ipc/bin/generic_ipc
```

启动时会从同一输出树的 `lib/hal.host_x86.so` 加载 HAL。Camera、Codec 和 Audio
只由 SvcKit Media 内部加载，Serial 尚未服务化，由 Application 直接验证。
Application 通过 `createMediaPipeline()` 同时启动一条 320×240@15fps 的 H.264
视频管线和一条 16kHz 单声道 G.711A 音频管线。应用将异步 Video/Audio Sink
注册到 Pipeline，两路各收到至少 10 个编码包后停止。`libhardware` 会自动搜索可执行文件旁边的
`../lib`，所以使用标准 `bin/lib/etc` 布局时不需要设置额外环境变量。

也可以使用环境变量统一替换配置目录，或分别通过命令行覆盖配置文件：

```bash
DARKOS_CONFIG_DIR=/path/to/etc ./output/generic_ipc/bin/generic_ipc

./output/generic_ipc/bin/generic_ipc \
  --board-config /path/to/board.json \
  --app-config /path/to/app.json
```

HAL 不在标准布局中时，可以覆盖 variant 和搜索目录：

```bash
DARKOS_HAL_VARIANT=host_x86 \
DARKOS_HAL_LIBRARY_PATH=/path/to/hal/lib \
./output/generic_ipc/bin/generic_ipc
```

`boards/ubuntu_x86_64_host.json` 中的 `/dev/ttyS0` 和 `/dev/ttyS1` 是示例设备节点。
接入真实串口后，应按 Ubuntu 的枚举结果改成实际的 `/dev/ttyUSB*`、
`/dev/ttyACM*` 或其他设备节点。

当前应用完成 Board/App 配置解析、Schema 校验、串口资源存在性、波特率和独占冲突
校验，并通过 SvcKit Media 验证 Camera→Codec 视频和 Audio→G.711A 编码管线；尚未
打开真实串口或启动 PTZ、控制协议服务。
