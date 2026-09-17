# Protocol services

`protocols/` 是面向应用的协议服务层，不是 SvcKit 子系统的一部分。协议实现只能组合
`subsystems/` 提供的稳定能力，不能直接访问 Platform/HAL，也不能反向成为 Media、
Network、Storage、Alarm 或 Peripheral 的依赖。

应用按需链接具体协议目标：

- `DarkOS::Protocol::RTSP`：第一阶段可用实现，提供 H.264 RTSP/RTP 服务。
- `DarkOS::Protocol::ONVIF`：设备发现、设备管理和媒体配置的接口骨架。
- `DarkOS::Protocol::GB28181`：SIP 注册、目录和实时流控制的接口骨架。
- `DarkOS::Protocol::RTMP`：推流接口骨架。
- `DarkOS::Protocol::UART`：基于 Peripheral 串口能力的协议接口骨架。
- `DarkOS::Protocol::Web`：HTTP/Web 管理服务接口骨架。

`DarkOS::Protocols` 仅用于标识协议层，不会自动链接所有协议。应用应显式声明实际使用
的目标，避免未使用协议进入最终镜像。

## RTSP 第一阶段

当前 RTSP 服务支持 `OPTIONS`、`DESCRIBE`、`SETUP`、`PLAY`、`PAUSE`、
`TEARDOWN`、`GET_PARAMETER` 和 `SET_PARAMETER`，提供：

- RTP over RTSP/TCP、RTP/UDP 单播及 IPv4 UDP 组播；
- Annex-B H.264（RFC 6184）和 G.711 A-law/u-law 音频轨；
- 视频、音频 RTCP Sender Report 与 SDES CNAME；
- 可选 RTSP Digest/MD5 鉴权；
- 可配置会话超时及自动资源回收；
- 组播单发布者约束，避免多个 RTSP 控制连接向同一组播地址重复发包。

这些能力都保留在 RTSP 模块内部，不下沉到 Media 或 Network 子系统。
