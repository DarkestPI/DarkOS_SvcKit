#pragma once

#include "media_types.h"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace darkos::media {

/**
 * 编码包的非拥有视图。
 *
 * data 只在 PacketCallback 调用期间有效；需要异步保存时，调用方必须复制。
 * 该约束阻止 Platform 的 dma-buf/MB_BLK 等厂商句柄泄漏到协议层。
 */
struct EncodedPacketView {
  const std::uint8_t *data{nullptr};
  std::size_t size{0};
  std::uint64_t timestampNs{0};
  VideoCodec codec{VideoCodec::H264};
  bool keyframe{false};
};

/** Camera/Decoder 产生的原始视频帧；opaque 只允许在同步调用链内透传。 */
struct VideoFrameView {
  const std::uint8_t *data{nullptr};
  std::size_t size{0};
  int fd{-1};
  std::uint64_t timestampNs{0};
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint32_t stride{0};
  PixelFormat pixelFormat{PixelFormat::Nv12};
  void *opaque{nullptr};
};

/**
 * 麦克风 PCM 数据的非拥有视图。
 *
 * data 只在 AudioFrameCallback 调用期间有效；需要跨线程消费时必须复制。
 * frameCount 是每个声道的采样帧数，不是所有声道样本数量之和。
 */
struct AudioFrameView {
  const std::uint8_t *data{nullptr};
  std::size_t size{0};
  std::uint64_t timestampNs{0};
  std::uint32_t sampleRate{0};
  std::uint32_t channelCount{0};
  std::uint32_t frameCount{0};
  AudioSampleFormat sampleFormat{AudioSampleFormat::PcmS16Le};
};

/** 音频编码包的非拥有视图。 */
struct EncodedAudioPacketView {
  const std::uint8_t *data{nullptr};
  std::size_t size{0};
  std::uint64_t timestampNs{0};
  AudioCodec codec{AudioCodec::Pcm};
  std::uint32_t sampleRate{0};
  std::uint32_t channelCount{0};
};

using VideoFrameCallback = std::function<void(const VideoFrameView &)>;
using PacketCallback = std::function<void(const EncodedPacketView &)>;
using AudioFrameCallback = std::function<void(const AudioFrameView &)>;
using AudioPacketCallback = std::function<void(const EncodedAudioPacketView &)>;

} // namespace darkos::media
