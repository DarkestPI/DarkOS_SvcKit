# ONVIF

M3 里程碑。

- SOAP（gSOAP 底库）+ WS-Discovery 设备发现
- Profile S：能力协商 / 码流 URI 下发（出流本身复用 Protocol/rtsp）
- PTZ / 事件订阅按需后补

## 映射到 ControlService 设备模型

ONVIF 是控制面的一个前端（docs/控制面设计.md 第 6 节）：SOAP 请求翻译到
Services/control 的 ControlService，不含业务判断。

- 预览：GetStreamUri 回现有 RTSP URL（rtsp://<ip>:8554/live），出流复用
  Protocol/rtsp，不另起媒体通道
- Imaging（亮度/对比度/宽动态等）：翻译到参数表的 imaging.* 参数
- Device（设备信息/网络/账号）：翻译到 device.* / rtsp.user 等参数；
  reboot 生效的参数走 ControlService 的 reboot 语义（落盘 + 提示）
