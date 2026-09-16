#pragma once

#include "media_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace darkos::media {

/** 不可变、有所有权的媒体数据；最后一个消费者释放后自动回收。 */
class MediaBuffer final {
public:
  explicit MediaBuffer(std::vector<std::uint8_t> bytes);

  const std::uint8_t *data() const noexcept;
  std::size_t size() const noexcept;

private:
  std::vector<std::uint8_t> bytes_;
};

using MediaBufferPtr = std::shared_ptr<const MediaBuffer>;
MediaBufferPtr copyMediaBuffer(const void *data, std::size_t size) noexcept;

/** 所有时间戳均为 CLOCK_MONOTONIC 纳秒。 */
struct VideoFrame {
  MediaBufferPtr buffer;
  std::uint64_t timestampNs{0};
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint32_t stride{0};
  PixelFormat pixelFormat{PixelFormat::Nv12};
};

struct VideoPacket {
  MediaBufferPtr buffer;
  std::uint64_t timestampNs{0};
  VideoCodec codec{VideoCodec::H264};
  bool keyframe{false};
};

struct AudioFrame {
  MediaBufferPtr buffer;
  std::uint64_t timestampNs{0};
  std::uint32_t sampleRate{0};
  std::uint32_t channelCount{0};
  std::uint32_t frameCount{0};
  AudioSampleFormat sampleFormat{AudioSampleFormat::PcmS16Le};
};

struct AudioPacket {
  MediaBufferPtr buffer;
  std::uint64_t timestampNs{0};
  AudioCodec codec{AudioCodec::Pcm};
  std::uint32_t sampleRate{0};
  std::uint32_t channelCount{0};
};

using VideoFramePtr = std::shared_ptr<const VideoFrame>;
using VideoPacketPtr = std::shared_ptr<const VideoPacket>;
using AudioFramePtr = std::shared_ptr<const AudioFrame>;
using AudioPacketPtr = std::shared_ptr<const AudioPacket>;

} // namespace darkos::media
