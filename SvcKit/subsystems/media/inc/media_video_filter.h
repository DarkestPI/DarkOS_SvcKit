#pragma once

#include "media_buffer.h"

namespace darkos::media {

/** 原始视频处理节点；输出 view 生命周期由具体 Filter 文档约定。 */
class VideoFilter {
public:
  virtual ~VideoFilter() = default;
  virtual int process(const VideoFrameView &input, VideoFrameView &output) = 0;

protected:
  VideoFilter() = default;
};

} // namespace darkos::media
