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

默认运行有限的音视频、存储、事件与网络状态探针并退出。启动常驻采集服务：

```bash
./output/generic_ipc/bin/generic_ipc --serve
# 可选指定 RTSP 端口以及录像、报警持久化目录
./output/generic_ipc/bin/generic_ipc --serve \
  --rtsp-port 8554 \
  --storage-dir /data/generic_ipc
```

服务通过 SvcKit EventLoop 消费 Media 与 Network 事件；收到 SIGINT/SIGTERM 后
按反向生命周期停止。编码 H.264 送往 Storage Sink，录像默认保存在输出树的
`data/recordings/`；报警日志保存为 `data/alarms.journal`。同一视频流通过新的
`SvcKit/protocols/rtsp` 发布，默认拉流地址为 `rtsp://127.0.0.1:8554/live`。
遗留的 `SvcKit/protocol` 已删除；generic_ipc 只显式链接当前使用的 RTSP 协议，
不会引入 ONVIF、GB28181 等尚未实现的协议骨架。

启动时 SvcKit 会从同一输出树的 `lib/hal.host_x86.so` 加载 HAL。Application
不直接引用 Platform HAL；Camera、Codec 和 Audio 由 SvcKit Media 管理，Wi-Fi
由 SvcKit Network 管理，Serial 设备访问由 SvcKit Peripheral 封装。
Application 通过 `createMediaPipeline()` 同时启动一条 320×240@15fps 的 H.264
视频管线和一条 16kHz 单声道 G.711A 音频管线。应用将异步 Video/Audio Sink
注册到 Pipeline，两路各收到至少 3 个编码包后停止。`libhardware` 会自动搜索可执行文件旁边的
`../lib`，所以使用标准 `bin/lib/etc` 布局时不需要设置额外环境变量。

启动过程中还会通过 SvcKit Network 输出接口、链路及地址快照，并消费 Media
Pipeline 的 Created→Starting→Running→Stopping→Stopped 生命周期事件。网络掉线、
网络监视错误和媒体管线失败会转换为持久化 Alarm 事件。受限容器
若禁止 netlink，网络快照会降级为告警，不影响本地媒体探针运行。

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
校验，并通过 SvcKit Media 验证 Camera→Codec 视频和 Audio→G.711A 编码管线，
同时验证编码视频落盘、索引及容量统计。
