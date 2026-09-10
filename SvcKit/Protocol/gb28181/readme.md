# 国标协议 GB/T 28181

M2 里程碑。注意它不是"RTSP 的变体"，是完整的独立协议栈：

- 信令：SIP（REGISTER/INVITE/MESSAGE，底库选型：eXosip 或自研最小事务层，届时决策）
- 媒体：H.264 → **PS 封装** → RTP（PS muxer 自研，live555 无此能力）
- SDP 国标方言：`y=`（SSRC）、`s=Play/Playback/Download`、回放 `u=` 时间段
- 会话管理：REGISTER 保活、心跳超时、invite/bye 生命周期

## 映射到 ControlService 设备模型

GB28181 是控制面的一个前端（docs/控制面设计.md 第 6 节）：国标控制命令
翻译到 Services/control 的 ControlService，不含业务判断。

- 预览：SIP 注册 + INVITE 后 PS 流推送，媒体封装走 SvcKit/stream 层
  （PS muxer），帧源复用 MediaService
- 控制命令（设备信息查询/配置、云台等）：翻译到参数表（device.* /
  video{ch}.* / ptz.*），校验与持久化由 ControlService 统一处理
