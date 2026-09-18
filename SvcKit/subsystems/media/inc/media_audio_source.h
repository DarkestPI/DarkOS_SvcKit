#pragma once

#include "media_buffer.h"
#include "media_types.h"

#include <functional>
#include <string>

namespace darkos::media {

/** PCM 音频生产节点。负责 HAL 读取、缓冲扩容和固定时长分块。 */
class AudioSource {
public:
  virtual ~AudioSource() = default;

  AudioSource(const AudioSource &) = delete;
  AudioSource &operator=(const AudioSource &) = delete;

  using FrameHandler = std::function<void(AudioFramePtr)>;
  using ErrorHandler = std::function<void(int, const std::string &)>;

  virtual int start(FrameHandler handler, ErrorHandler errorHandler) = 0;
  virtual int stop() = 0;
  virtual bool running() const noexcept = 0;
  virtual const AudioCaptureConfig &format() const noexcept = 0;

protected:
  AudioSource() = default;
};

/** 已编码音频生产节点。平台可直接提供 AI->AENC 等硬件访问单元。 */
class AudioPacketSource {
public:
  virtual ~AudioPacketSource() = default;

  AudioPacketSource(const AudioPacketSource &) = delete;
  AudioPacketSource &operator=(const AudioPacketSource &) = delete;

  using PacketHandler = std::function<void(AudioPacketPtr)>;
  using ErrorHandler = std::function<void(int, const std::string &)>;

  virtual int start(PacketHandler handler, ErrorHandler errorHandler) = 0;
  virtual int stop() = 0;
  virtual bool running() const noexcept = 0;
  virtual const AudioEncoderConfig &config() const noexcept = 0;

protected:
  AudioPacketSource() = default;
};

} // namespace darkos::media
