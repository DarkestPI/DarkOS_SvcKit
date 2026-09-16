#pragma once

#include "media_audio_sink.h"
#include "media_types.h"
#include "media_video_sink.h"

#include <cstdint>
#include <memory>
#include <string>

namespace darkos::media {

using SinkId = std::uint64_t;

/**
 * 音视频节点图的生命周期门面。
 *
 * 数据通过拥有生命周期的 packet 和异步 Sink 分发；Pipeline 不在 Camera、
 * Audio 或 Encoder 线程中执行 Application 代码。控制面事件通过 waitEvent()
 * 拉取，避免错误回调重入生命周期方法。
 */
class MediaPipeline {
public:
  virtual ~MediaPipeline() = default;

  MediaPipeline(const MediaPipeline &) = delete;
  MediaPipeline &operator=(const MediaPipeline &) = delete;

  virtual int addVideoSink(std::shared_ptr<VideoPacketSink> sink,
                           const MediaQueueConfig &queue, SinkId &id) = 0;
  virtual int addAudioSink(std::shared_ptr<AudioPacketSink> sink,
                           const MediaQueueConfig &queue, SinkId &id) = 0;
  virtual int removeVideoSink(SinkId id) = 0;
  virtual int removeAudioSink(SinkId id) = 0;

  virtual int start() = 0;
  /** 不应从 Sink::consume() 调用；检测到该重入时返回 -EDEADLK。 */
  virtual int stop() = 0;
  virtual PipelineState state() const noexcept = 0;
  virtual bool running() const noexcept = 0;
  virtual PipelineStats stats() const noexcept = 0;

  /** timeoutMs < 0 永久等待，0 立即返回；超时返回 -ETIMEDOUT。 */
  virtual int waitEvent(MediaEvent &event, int timeoutMs) = 0;

protected:
  MediaPipeline() = default;
};

std::unique_ptr<MediaPipeline>
createMediaPipeline(const VideoPipelineConfig &config, std::string &error);

std::unique_ptr<MediaPipeline>
createMediaPipeline(const MediaPipelineConfig &config, std::string &error);

} // namespace darkos::media
