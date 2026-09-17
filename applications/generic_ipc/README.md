# Generic IPC（Ubuntu x86_64）

`generic_ipc` 是 Ubuntu x86_64 上的 IPC 参考应用。目前包含：

- Camera → H.264 与 Audio → G.711A 媒体管线
- H.264 + G.711A RTSP 服务
- RTSP Digest 鉴权、TCP/UDP 单播、UDP 组播、RTCP Sender Report
- 连续录像、报警日志、网络状态与会话超时回收

应用只调用 SvcKit，不直接依赖 Platform HAL。宿主机运行时会加载
`hal.host_x86.so`，因此不需要 Rockchip 交叉工具链或真实摄像头。

## 构建

在仓库根目录执行：

```bash
cmake --preset generic_ipc
cmake --build --preset generic_ipc --parallel
```

输出位于：

```text
output/generic_ipc/
├── bin/generic_ipc
├── etc/app.json
├── etc/board.json
└── lib/hal.host_x86.so
```

构建时会把 `applications/generic_ipc/etc/app.json` 复制到输出目录。修改源配置后，
需要重新执行构建命令；也可以用 `--app-config` 直接加载另一份配置。

## 快速运行

启动常驻采集、录像和 RTSP 服务：

```bash
./output/generic_ipc/bin/generic_ipc --serve
```

默认拉流地址：

```bash
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/live
```

也可以验证 UDP 单播：

```bash
ffplay -rtsp_transport udp rtsp://127.0.0.1:8554/live
```

按 `Ctrl+C` 停止服务。录像默认写入 `output/generic_ipc/data/recordings/`，报警日志
写入 `output/generic_ipc/data/alarms.journal`。

不带 `--serve` 时，程序只执行一次配置、网络、音视频、存储探针，然后自行退出：

```bash
./output/generic_ipc/bin/generic_ipc
```

## RTSP 配置

RTSP 参数统一放在 `applications/generic_ipc/etc/app.json`：

```json
{
  "rtsp": {
    "enabled": true,
    "bind_address": "0.0.0.0",
    "port": 8554,
    "mount_path": "live",
    "session_timeout_seconds": 60,
    "rtcp_report_interval_ms": 5000,
    "maximum_rtp_payload_bytes": 1200,
    "maximum_client_backlog_bytes": 2097152,
    "authentication": {
      "username": "",
      "password_env": "DARKOS_RTSP_PASSWORD"
    },
    "multicast": {
      "enabled": false,
      "address": "239.255.0.1",
      "video_port": 5004,
      "audio_port": 5006,
      "ttl": 16
    }
  }
}
```

主要参数：

| 参数 | 说明 |
| --- | --- |
| `enabled` | 是否启动 RTSP 服务 |
| `bind_address` / `port` | 监听地址和端口 |
| `mount_path` | URL 路径，不含 `/` |
| `session_timeout_seconds` | 无活动会话的回收时间 |
| `rtcp_report_interval_ms` | RTCP Sender Report 周期 |
| `authentication.username` | Digest 用户名；空字符串表示关闭鉴权 |
| `authentication.password_env` | 保存密码的环境变量名，密码不写入 JSON |
| `multicast.enabled` | 是否允许客户端通过 UDP 组播接收 |
| `multicast.video_port` | 视频 RTP 端口；对应 RTCP 端口为该值加 1 |
| `multicast.audio_port` | 音频 RTP 端口；对应 RTCP 端口为该值加 1 |

### 开启鉴权

把 `authentication.username` 改为 `admin`，重新构建，然后通过配置指定的环境变量
注入密码：

```bash
DARKOS_RTSP_PASSWORD='change-me' \
  ./output/generic_ipc/bin/generic_ipc --serve

ffplay -rtsp_transport tcp \
  'rtsp://admin:change-me@127.0.0.1:8554/live'
```

用户名非空但密码环境变量未设置时，应用会拒绝启动，避免意外开放无鉴权服务。

### 开启组播

把 `multicast.enabled` 改为 `true`，重新构建并启动，然后执行：

```bash
ffplay -rtsp_transport udp_multicast rtsp://127.0.0.1:8554/live
```

默认视频使用 `239.255.0.1:5004/5005`，音频使用
`239.255.0.1:5006/5007`（RTP/RTCP）。启用组播不会关闭 TCP 和 UDP 单播。

## 运行时覆盖

临时测试时可覆盖部分参数，无需修改 JSON：

```bash
DARKOS_RTSP_USERNAME=admin \
DARKOS_RTSP_PASSWORD='change-me' \
DARKOS_RTSP_MULTICAST_ADDRESS=239.255.0.2 \
./output/generic_ipc/bin/generic_ipc --serve --rtsp-port 9554
```

优先级为：命令行 `--rtsp-port` > RTSP 环境变量 > `app.json`。其他常用参数：

```bash
./output/generic_ipc/bin/generic_ipc --serve \
  --app-config /path/to/app.json \
  --board-config /path/to/board.json \
  --storage-dir /path/to/data
```

也可以用 `DARKOS_CONFIG_DIR=/path/to/etc` 同时替换默认的 `app.json` 和
`board.json` 所在目录。HAL 不在标准输出布局中时，使用：

```bash
DARKOS_HAL_VARIANT=host_x86 \
DARKOS_HAL_LIBRARY_PATH=/path/to/hal/lib \
./output/generic_ipc/bin/generic_ipc --serve
```

`boards/ubuntu_x86_64_host.json` 中的 `/dev/ttyS0` 和 `/dev/ttyS1` 只是示例。
接入真实串口时，请改成系统实际枚举出的 `/dev/ttyUSB*`、`/dev/ttyACM*` 或其他
设备节点。
