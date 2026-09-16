#pragma once

#include "media_buffer.h"
#include "media_types.h"

namespace darkos::media {

/** 原始视频帧到拥有生命周期的编码包。 */
class VideoEncoder {
public:
  virtual ~VideoEncoder() = default;

  VideoEncoder(const VideoEncoder &) = delete;
  VideoEncoder &operator=(const VideoEncoder &) = delete;

  virtual int start() = 0;
  virtual int stop() = 0;
  virtual bool running() const noexcept = 0;
  virtual int encode(const VideoFrame &frame, VideoPacketPtr &packet) = 0;
  virtual const VideoEncoderConfig &config() const noexcept = 0;

protected:
  VideoEncoder() = default;
};

/** 编码视频包到原始帧的转换节点；无输出时返回 -EAGAIN。 */
class VideoDecoder {
public:
  virtual ~VideoDecoder() = default;

  VideoDecoder(const VideoDecoder &) = delete;
  VideoDecoder &operator=(const VideoDecoder &) = delete;

  virtual int start() = 0;
  virtual int stop() = 0;
  virtual bool running() const noexcept = 0;
  virtual int decode(const VideoPacket &packet, VideoFramePtr &frame) = 0;

protected:
  VideoDecoder() = default;
};

} // namespace darkos::media
