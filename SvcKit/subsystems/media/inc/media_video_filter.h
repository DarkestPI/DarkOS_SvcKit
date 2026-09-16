#pragma once

#include "media_buffer.h"

namespace darkos::media {

/** 原始视频处理节点；输入输出均持有不可变 buffer 的共享所有权。 */
class VideoFilter {
public:
  virtual ~VideoFilter() = default;
  virtual int process(VideoFramePtr input, VideoFramePtr &output) = 0;

protected:
  VideoFilter() = default;
};

} // namespace darkos::media
