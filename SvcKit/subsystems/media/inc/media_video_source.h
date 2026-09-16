#pragma once

#include "media_buffer.h"
#include "media_types.h"

#include <functional>
#include <string>

namespace darkos::media {

/** 原始视频帧生产节点。回调在 Source 自己的采集线程同步执行。 */
class VideoSource {
public:
  virtual ~VideoSource() = default;

  VideoSource(const VideoSource &) = delete;
  VideoSource &operator=(const VideoSource &) = delete;

  using FrameHandler = std::function<void(VideoFramePtr)>;
  using ErrorHandler = std::function<void(int, const std::string &)>;

  /** handler 只负责非阻塞入队；帧本身拥有数据生命周期。 */
  virtual int start(FrameHandler handler, ErrorHandler errorHandler) = 0;
  virtual int stop() = 0;
  virtual bool running() const noexcept = 0;
  virtual const VideoCaptureConfig &format() const noexcept = 0;

protected:
  VideoSource() = default;
};

} // namespace darkos::media
