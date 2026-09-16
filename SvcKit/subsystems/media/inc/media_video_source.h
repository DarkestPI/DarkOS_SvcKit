#pragma once

#include "media_buffer.h"
#include "media_types.h"

namespace darkos::media {

/** 原始视频帧生产节点。回调在 Source 自己的采集线程同步执行。 */
class VideoSource {
public:
  virtual ~VideoSource() = default;

  VideoSource(const VideoSource &) = delete;
  VideoSource &operator=(const VideoSource &) = delete;

  virtual int start(VideoFrameCallback callback) = 0;
  virtual int stop() = 0;
  virtual bool running() const noexcept = 0;
  virtual const VideoCaptureConfig &format() const noexcept = 0;

protected:
  VideoSource() = default;
};

} // namespace darkos::media
