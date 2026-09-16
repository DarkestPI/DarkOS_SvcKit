#pragma once

#include "media_buffer.h"

namespace darkos::media {

/** Display 等消费原始视频帧的终点。 */
class VideoFrameSink {
public:
  virtual ~VideoFrameSink() = default;
  virtual int start() = 0;
  virtual int stop() = 0;
  virtual int consume(const VideoFrameView &frame) = 0;

protected:
  VideoFrameSink() = default;
};

/** RTSP、录像等消费编码视频包的终点。 */
class VideoPacketSink {
public:
  virtual ~VideoPacketSink() = default;
  virtual int start() = 0;
  virtual int stop() = 0;
  virtual int consume(const EncodedPacketView &packet) = 0;

protected:
  VideoPacketSink() = default;
};

} // namespace darkos::media
