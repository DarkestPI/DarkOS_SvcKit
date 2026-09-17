# 网络通讯

## 当前已实现

- 公共 `EventLoop`：epoll 一次性监听、跨线程任务、周期/单次定时器；
- IPv4/IPv6 地址解析和文本转换；
- TCP/UDP RAII Socket、非阻塞模式和 TCP acceptor；
- Linux 网络接口快照；
- netlink 链路、地址变化事件；
- SvcKit Wi-Fi 门面及 Platform Wi-Fi HAL 适配（启停、扫描、连接、状态）；
- host 回环、异步 TCP、NetworkManager 和 Wi-Fi 端到端测试。

## 边界

本次交付范围是 generic IPC 必需的网络竖向链路：系统网络状态、异步 TCP/UDP 和
Wi-Fi 控制。旧 `SvcKit/protocol` 不属于本子系统依赖。目录中的 HTTP、MQTT、
WebSocket、TLS/DTLS、4G、路由管理、
连接池和网络诊断源文件仍是预留骨架，未加入构建，不对外宣称已实现。

## 目标接入模式

- 有线网络
- WIFI 无线网络
- 4G 无线网络
