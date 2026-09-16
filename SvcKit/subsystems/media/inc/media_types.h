#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace darkos::media {

/** SvcKit 支持的编码格式。数值不与任何厂商 SDK ABI 绑定。 */
enum class VideoCodec : std::uint32_t {
  H264,
  H265,
  Mjpeg,
};

/** SvcKit 媒体管线使用的原始图像格式。 */
enum class PixelFormat : std::uint32_t {
  Nv12,
  Nv21,
  Yuyv,
  Rgb24,
};

/** 音频采样格式；当前 Platform Audio SPI 只统一呈现 S16LE PCM。 */
enum class AudioSampleFormat : std::uint32_t {
  PcmS16Le,
};

/** SvcKit 音频编码格式。Platform Audio SPI 始终只负责 PCM I/O。 */
enum class AudioCodec : std::uint32_t {
  Pcm,
  G711A,
  G711U,
  Aac,
  Opus,
};

/** 采集端配置；cameraId 遵循 Platform Camera SPI 的 camera/cameraN 约定。 */
struct VideoCaptureConfig {
  std::string cameraId{"camera"};
  std::uint32_t width{1920};
  std::uint32_t height{1080};
  std::uint32_t fps{25};
  PixelFormat pixelFormat{PixelFormat::Nv12};
};

/** 编码端配置。 */
struct VideoEncoderConfig {
  std::string codecId{"codec"};
  VideoCodec codec{VideoCodec::H264};
  std::uint32_t bitrateBps{2'000'000};
  std::uint32_t gop{50};
};

/** 一路 Camera→Codec 管线配置。 */
struct VideoPipelineConfig {
  VideoCaptureConfig capture;
  VideoEncoderConfig encoder;

  /** 编码输出暂存区；0 表示由实现根据实际分辨率计算。 */
  std::size_t encodedBufferCapacity{0};
};

/** 麦克风采集配置。framesPerBuffer 决定每次回调承载的采样帧数。 */
struct AudioCaptureConfig {
  std::uint32_t sampleRate{16'000};
  std::uint32_t channelCount{1};
  AudioSampleFormat sampleFormat{AudioSampleFormat::PcmS16Le};
  std::uint32_t framesPerBuffer{320}; // 16 kHz 时为 20 ms
};

/** 音频编码配置；frameDurationMs 用于确定编码器每包采样点数。 */
struct AudioEncoderConfig {
  AudioCodec codec{AudioCodec::G711A};
  std::uint32_t bitrateBps{64'000};
  std::uint32_t frameDurationMs{20};
};

/** 一路 Camera→Codec 视频和一路麦克风 PCM 采集的音视频管线配置。 */
struct MediaPipelineConfig {
  VideoPipelineConfig video;
  AudioCaptureConfig audio;
};

} // namespace darkos::media
