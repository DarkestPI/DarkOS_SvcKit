# SvcKit Media

SvcKit Media 是 Application 与协议层使用的媒体门面，负责节点编排、数据所有权、
异步队列、背压、Fanout、状态事件与统计。它只依赖 Platform 公共 SPI，不包含 SoC
SDK 调用。

```text
Application / Protocol / Recorder / RTSP
                   ↓
        SvcKit Media Pipeline + Sink
                   ↓
      Platform Camera / Codec / Audio SPI
                   ↓
        host_x86 或 vendors/<vendor>/socs/<soc>
```

## 已落地的数据流

```text
VideoSource → bounded queue → VideoEncoder → Fanout → VideoPacketSink*
AudioSource → bounded queue → AudioEncoder → Fanout → AudioPacketSink*
```

- Source 回调只创建有所有权的不可变 `MediaBuffer` 并非阻塞入队，不执行应用代码；
- 音视频各有独立编码线程，音频编码器是 Pipeline 内部节点；
- 每个 Sink 有独立有界队列和消费线程，慢消费者按配置丢最旧或最新包；
- 同一个 `shared_ptr<const MediaBuffer>` 可安全交给 RTSP、录像和分析等多个消费者；
- 所有时间戳统一为 `CLOCK_MONOTONIC` 纳秒；
- 生命周期使用 `PipelineState`，错误和丢帧通过 `waitEvent()` 拉取，避免回调重入；
- `stats()` 提供采集、编码、丢帧和错误计数。

## API 示例

```cpp
darkos::media::MediaPipelineConfig config;
config.video.encoder.codec = darkos::media::VideoCodec::H264;
config.audio.encoder.codec = darkos::media::AudioCodec::G711A;

std::string error;
auto pipeline = darkos::media::createMediaPipeline(config, error);

auto videoSink = darkos::media::createVideoProbeSink();
auto audioSink = darkos::media::createAudioProbeSink();

darkos::media::SinkId videoId = 0;
darkos::media::SinkId audioId = 0;
darkos::media::MediaQueueConfig queue{
    8, darkos::media::BackpressurePolicy::DropOldest};
pipeline->addVideoSink(videoSink, queue, videoId);
pipeline->addAudioSink(audioSink, queue, audioId);
pipeline->start();
videoSink->waitForPackets(10, 5'000);
audioSink->waitForPackets(10, 5'000);
```

Probe Sink 是无回调的自检节点，通过 `waitForPackets()` 等待，通过 `snapshot()`
读取统计。正式的 RTSP、Recorder、Display 等组件应实现对应 Sink 接口；它们的
`consume()` 由 Pipeline 的 Sink 工作线程调用，而不是 Camera、Audio 或 Encoder
线程。

## 公共节点

| 节点 | 状态 |
| --- | --- |
| `VideoSource` | Platform Camera 适配，支持 host 合成/UVC 和 RV1126B |
| `AudioSource` | Platform Audio Input 适配、动态扩容和固定帧分块 |
| `VideoEncoder` | Platform Codec 适配，host x264 / RV1126B VENC |
| `AudioEncoder/Decoder` | PCM、G.711A、G.711U；AAC/Opus 明确返回未支持 |
| `VideoDecoder` | 接口已定义，Platform decode 适配待接 |
| `VideoFilter/AudioFilter` | 接口已定义，OSD/AEC/重采样实现按能力加入 |
| `VideoSink/AudioSink` | 原始帧和编码包 Sink 分离；已提供无回调 Probe Sink |
| `MediaMuxer` | 已定义音视频写入契约，MP4/TS 实现待接 |
| `MediaManager` | 无全局单例的管线创建门面；后续承载资源仲裁 |

编解码接口按媒体类型对称组织：`media_video_codec.h` 同时定义 VideoEncoder 和
VideoDecoder，`media_audio_codec.h` 同时定义 AudioEncoder 和 AudioDecoder。

## 分层边界

SvcKit Media 负责产品级管线、生命周期、队列、背压、分发与统计。Platform/SoC
适配层负责 V4L2、RKAIQ、MPI、dma-buf、VI/VENC 资源以及未来的通用硬件直连。
SvcKit、Application 和协议层禁止出现 `RK_MPI_*`、`MB_BLK` 或 Rockchip 通道号。

无法提供 AAC、Opus、MP4 等能力时，工厂必须明确失败，不能返回成功的空实现。
更完整的分层约束见 `docs/Platform与SvcKit媒体分层设计.md`。
