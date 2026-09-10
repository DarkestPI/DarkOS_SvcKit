# Web 前端骨架（占位）：静态页面托管 + 私有 HTTP REST API + WebRTC 信令，
# 三件事共用同一自研 HTTP 服务器底座（挂 EventLoop），全部翻译到 ControlService。
# 本期不实现，见 readme.md 与 docs/控制面设计.md 第 5 节。
add_library(darkos_web INTERFACE)
add_library(DarkOS::Web ALIAS darkos_web)
