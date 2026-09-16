# SvcKit Media

SvcKit Media 是 Application 与协议层使用的媒体门面。它负责媒体生命周期、
Camera/Codec 组合、缓冲、线程调度和帧分发，但不直接包含任何 SoC SDK 调用。

依赖方向固定为：

```text
Application / Protocol
          ↓
     SvcKit Media node graph
          ↓
Platform Camera / Codec / Audio SPI
          ↓
host_x86 或 vendors/<vendor>/socs/<soc>
```

当前落地的第一条竖向能力是 `createMediaPipeline()`：通过 Camera/Codec SPI 创建
一路采集编码视频，并可通过 Audio SPI 同步启动一路麦克风 PCM 采集。视频以
`EncodedPacketView`、音频以 `AudioFrameView` 回调交付。未来录像、RTSP、
GB28181 和分析服务应消费该门面，而不是直接持有 Platform HAL device。

`MediaPipeline` 是便捷门面，不是所有功能的实现容器。实际运行链路由节点组成：

```text
PlatformVideoSource → PlatformVideoEncoder → PacketCallback / VideoPacketSink
PlatformAudioSource ───────────────────────→ AudioFrameCallback / AudioEncoder
```

Source 负责产生数据，Codec 负责格式转换，Filter 负责处理，Sink/Muxer 负责消费，
Pipeline 只管理节点启动顺序、停止顺序和失败回滚。所有公共接口统一位于
`darkos::media` 命名空间。

编解码接口按媒体类型对称组织：

```text
media_video_codec.h → VideoEncoder + VideoDecoder
media_audio_codec.h → AudioEncoder + AudioDecoder
```

## 节点状态

| 节点 | 当前状态 |
| --- | --- |
| `VideoSource` | 已实现 Platform Camera 适配，支持 host 合成/UVC 和 RV1126B |
| `AudioSource` | 已实现 Platform Audio Input 适配、动态扩容和固定帧分块 |
| `VideoEncoder` | 已实现 Platform Codec 适配，host x264 / RV1126B VENC |
| `AudioEncoder/Decoder` | 已实现 PCM、G.711A、G.711U；AAC/Opus 明确返回未支持 |
| `VideoDecoder` | 接口已稳定，Platform decode 适配尚未接入管线 |
| `VideoFilter/AudioFilter` | 接口已稳定，具体 OSD/AEC/重采样实现按能力加入 |
| `VideoSink/AudioSink` | 已区分原始帧 Sink 与编码包 Sink，具体协议/显示节点待接 |
| `MediaMuxer` | 已定义音视频时间戳写入契约，MP4/TS 实现待接 |
| `MediaManager` | 已实现无全局单例的管线创建门面，后续承载多路资源仲裁 |

## 边界

SvcKit Media 负责：

- 主辅码流、抓拍等产品级管线；
- Camera、Codec、Display 的组合与启动/停止顺序；
- buffer 生命周期、背压、分发与统计；
- 软件回退和 Platform 可选硬件加速能力的选择。

Platform/SoC 适配层负责：

- V4L2、Rockit、RKAIQ、MPI 等硬件机制；
- dma-buf/厂商 buffer 与统一 SPI 之间的转换；
- VI/VENC 等资源创建、销毁以及硬件直连。

禁止 SvcKit 和协议层出现 `RK_MPI_*`、`MB_BLK`、Rockchip 通道号等厂商类型。

## 当前 API

```cpp
#include <media_pipeline.h>

darkos::media::VideoPipelineConfig config;
std::string error;
auto pipeline = darkos::media::createMediaPipeline(
    config,
    [](const darkos::media::EncodedPacketView &packet) {
        // packet.data 仅在本次回调期间有效；异步消费时必须复制或转入 BufferPool。
    },
    error);
```

音视频管线使用 `MediaPipelineConfig` 和两个回调：

```cpp
darkos::media::MediaPipelineConfig config;
auto pipeline = darkos::media::createMediaPipeline(
    config,
    [](const darkos::media::EncodedPacketView &packet) {
        // H.264/H.265/MJPEG 编码视频。
    },
    [](const darkos::media::AudioFrameView &frame) {
        // 当前为 S16LE PCM；可交给内置 G.711A/G.711U 编码节点。
    },
    error);
```

不允许通过返回成功的空实现冒充未落地能力。工厂无法提供 AAC、Opus、MP4 等节点
时必须明确失败；调用方可以据此选择软件插件、降级格式或拒绝启动。

更完整的分层和演进约束见 `docs/Platform与SvcKit媒体分层设计.md`。
