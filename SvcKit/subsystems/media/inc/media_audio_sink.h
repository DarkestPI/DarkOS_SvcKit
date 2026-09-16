#pragma once

#include "media_buffer.h"

#include <cstdint>
#include <memory>

namespace darkos::media {

/** 扬声器等消费 PCM 的终点。 */
class AudioFrameSink {
public:
  virtual ~AudioFrameSink() = default;
  virtual int start() = 0;
  virtual int stop() = 0;
  virtual int consume(AudioFramePtr frame) = 0;

protected:
  AudioFrameSink() = default;
};

/** RTSP、录像等消费编码音频包的终点。 */
class AudioPacketSink {
public:
  virtual ~AudioPacketSink() = default;
  virtual int start() = 0;
  virtual int stop() = 0;
  virtual int consume(AudioPacketPtr packet) = 0;

protected:
  AudioPacketSink() = default;
};

struct AudioProbeStats {
  std::uint64_t packetCount{0};
  std::uint64_t byteCount{0};
  std::uint64_t lastTimestampNs{0};
  AudioCodec lastCodec{AudioCodec::Pcm};
  std::uint32_t lastSampleRate{0};
  std::uint32_t lastChannelCount{0};
};

/** 无回调的诊断 Sink，供自检和测试等待编码包并读取统计。 */
class AudioProbeSink : public AudioPacketSink {
public:
  virtual int waitForPackets(std::uint64_t minimum, int timeoutMs) = 0;
  virtual AudioProbeStats snapshot() const noexcept = 0;

protected:
  AudioProbeSink() = default;
};

std::shared_ptr<AudioProbeSink> createAudioProbeSink();

} // namespace darkos::media
