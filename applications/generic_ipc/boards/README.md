# Ubuntu x86_64 板级配置

`ubuntu_x86_64_host.json` 描述宿主机硬件资源，不描述 Pelco-D 等业务协议。
Application 的 `etc/app.json` 负责把业务服务绑定到这些资源。

`/dev/ttyS0`、`/dev/ttyS1` 是通用示例值。连接 USB 转串口后，通常需要按 Ubuntu
实际枚举结果改成 `/dev/ttyUSB*` 或 `/dev/ttyACM*`；如果硬件是 RS-485，也要把
`electrical` 改为 `rs485`，并确认内核或转接器如何控制收发方向。
