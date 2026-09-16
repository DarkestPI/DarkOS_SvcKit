#pragma once

#include "media_buffer.h"

#include <cstdint>
#include <memory>

namespace darkos::media {

/** Display 等消费原始视频帧的终点。 */
class VideoFrameSink {
public:
  virtual ~VideoFrameSink() = default;
  virtual int start() = 0;
  virtual int stop() = 0;
  virtual int consume(VideoFramePtr frame) = 0;

protected:
  VideoFrameSink() = default;
};

/** RTSP、录像等消费编码视频包的终点。 */
class VideoPacketSink {
public:
  virtual ~VideoPacketSink() = default;
  virtual int start() = 0;
  virtual int stop() = 0;
  virtual int consume(VideoPacketPtr packet) = 0;

protected:
  VideoPacketSink() = default;
};

struct VideoProbeStats {
  std::uint64_t packetCount{0};
  std::uint64_t byteCount{0};
  std::uint64_t keyframeCount{0};
  std::uint64_t lastTimestampNs{0};
  VideoCodec lastCodec{VideoCodec::H264};
};

/** 无回调的诊断 Sink，供自检和测试等待编码包并读取统计。 */
class VideoProbeSink : public VideoPacketSink {
public:
  virtual int waitForPackets(std::uint64_t minimum, int timeoutMs) = 0;
  virtual VideoProbeStats snapshot() const noexcept = 0;

protected:
  VideoProbeSink() = default;
};

std::shared_ptr<VideoProbeSink> createVideoProbeSink();

} // namespace darkos::media
