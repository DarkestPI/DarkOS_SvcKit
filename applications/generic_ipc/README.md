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

也可以使用环境变量统一替换配置目录，或分别通过命令行覆盖配置文件：

```bash
DARKOS_CONFIG_DIR=/path/to/etc ./output/generic_ipc/bin/generic_ipc

./output/generic_ipc/bin/generic_ipc \
  --board-config /path/to/board.json \
  --app-config /path/to/app.json
```

`boards/ubuntu_x86_64_host.json` 中的 `/dev/ttyS0` 和 `/dev/ttyS1` 是示例设备节点。
接入真实串口后，应按 Ubuntu 的枚举结果改成实际的 `/dev/ttyUSB*`、
`/dev/ttyACM*` 或其他设备节点。

当前应用完成 Board/App 配置解析、Schema 校验、串口资源存在性、波特率和独占冲突
校验；尚未打开真实串口或启动 PTZ、控制协议服务。
