# 串口控制面 UART

多客户端控制面的串口前端（docs/控制面设计.md 第 4 节）：HAL serial 透出 fd
挂 EventLoop，行协议（HELP/LIST/GET/SET/SAVE/REBOOT）翻译到 ControlService。
serial HAL 由 `Services/peripheral/SerialPort` 打开并拥有，外部宿主只把 fd 注入
UartAdapter；适配器不碰 HAL、不关闭 fd。校验/权限/持久化都在 ControlService。

- 设备路径/波特率和失败策略均由使用方宿主决定
- host 联调：`socat -d -d pty,raw,echo=0 pty,raw,echo=0` 建 PTY 对，
  或直接用 uart_probe（openpty 自测全命令）
