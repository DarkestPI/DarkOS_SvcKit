#pragma once

#include "media_buffer.h"

namespace darkos::media {

/** 扬声器等消费 PCM 的终点。 */
class AudioFrameSink {
public:
  virtual ~AudioFrameSink() = default;
  virtual int start() = 0;
  virtual int stop() = 0;
  virtual int consume(const AudioFrameView &frame) = 0;

protected:
  AudioFrameSink() = default;
};

/** RTSP、录像等消费编码音频包的终点。 */
class AudioPacketSink {
public:
  virtual ~AudioPacketSink() = default;
  virtual int start() = 0;
  virtual int stop() = 0;
  virtual int consume(const EncodedAudioPacketView &packet) = 0;

protected:
  AudioPacketSink() = default;
};

} // namespace darkos::media
