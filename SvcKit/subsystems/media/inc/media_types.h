#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace darkos::media {

enum class VideoCodec : std::uint32_t { H264, H265, Mjpeg };
enum class PixelFormat : std::uint32_t { Nv12, Nv21, Yuyv, Rgb24 };
enum class AudioSampleFormat : std::uint32_t { PcmS16Le };
enum class AudioCodec : std::uint32_t { Pcm, G711A, G711U, Aac, Opus };

enum class BackpressurePolicy : std::uint32_t {
  DropOldest,
  DropNewest,
};

struct MediaQueueConfig {
  std::size_t capacity{4};
  BackpressurePolicy policy{BackpressurePolicy::DropOldest};
};

enum class PipelineState : std::uint32_t {
  Created,
  Starting,
  Running,
  Degraded,
  Stopping,
  Stopped,
  Failed,
};

enum class MediaEventType : std::uint32_t {
  StateChanged,
  VideoFrameDropped,
  AudioFrameDropped,
  VideoEncodeError,
  AudioEncodeError,
  SourceError,
  SinkError,
};

/** 控制面事件。timestampNs 始终使用 CLOCK_MONOTONIC 纳秒时基。 */
struct MediaEvent {
  MediaEventType type{MediaEventType::StateChanged};
  PipelineState state{PipelineState::Created};
  int code{0};
  std::uint64_t timestampNs{0};
  std::string component;
  std::string message;
};

struct PipelineStats {
  std::uint64_t capturedVideoFrames{0};
  std::uint64_t encodedVideoPackets{0};
  std::uint64_t droppedVideoFrames{0};
  std::uint64_t videoEncodeErrors{0};
  std::uint64_t capturedAudioFrames{0};
  std::uint64_t encodedAudioPackets{0};
  std::uint64_t droppedAudioFrames{0};
  std::uint64_t audioEncodeErrors{0};
  std::uint64_t sinkErrors{0};
};

struct VideoCaptureConfig {
  std::string cameraId{"camera"};
  std::uint32_t width{1920};
  std::uint32_t height{1080};
  std::uint32_t fps{25};
  PixelFormat pixelFormat{PixelFormat::Nv12};
};

struct VideoEncoderConfig {
  std::string codecId{"codec"};
  VideoCodec codec{VideoCodec::H264};
  std::uint32_t bitrateBps{2'000'000};
  std::uint32_t gop{50};
};

struct VideoPipelineConfig {
  VideoCaptureConfig capture;
  VideoEncoderConfig encoder;
  MediaQueueConfig inputQueue;
  std::size_t encodedBufferCapacity{0};
};

struct AudioCaptureConfig {
  std::uint32_t sampleRate{16'000};
  std::uint32_t channelCount{1};
  AudioSampleFormat sampleFormat{AudioSampleFormat::PcmS16Le};
  std::uint32_t framesPerBuffer{320};
};

struct AudioEncoderConfig {
  AudioCodec codec{AudioCodec::G711A};
  std::uint32_t bitrateBps{64'000};
  std::uint32_t frameDurationMs{20};
};

struct AudioPipelineConfig {
  AudioCaptureConfig capture;
  AudioEncoderConfig encoder;
  MediaQueueConfig inputQueue{8, BackpressurePolicy::DropOldest};
};

struct MediaPipelineConfig {
  VideoPipelineConfig video;
  AudioPipelineConfig audio;
};

} // namespace darkos::media
