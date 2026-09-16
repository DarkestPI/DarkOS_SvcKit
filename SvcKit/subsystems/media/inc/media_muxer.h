#pragma once

#include "media_buffer.h"

namespace darkos::media {

/** MP4/TS 等音视频封装节点；write 调用保持各自时间戳。 */
class MediaMuxer {
public:
  virtual ~MediaMuxer() = default;
  virtual int open() = 0;
  virtual int writeVideo(const EncodedPacketView &packet) = 0;
  virtual int writeAudio(const EncodedAudioPacketView &packet) = 0;
  virtual int close() = 0;

protected:
  MediaMuxer() = default;
};

} // namespace darkos::media
