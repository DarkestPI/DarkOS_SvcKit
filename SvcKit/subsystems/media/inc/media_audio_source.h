#pragma once

#include "media_buffer.h"
#include "media_types.h"

namespace darkos::media {

/** PCM 音频生产节点。负责 HAL 读取、缓冲扩容和固定时长分块。 */
class AudioSource {
public:
  virtual ~AudioSource() = default;

  AudioSource(const AudioSource &) = delete;
  AudioSource &operator=(const AudioSource &) = delete;

  virtual int start(AudioFrameCallback callback) = 0;
  virtual int stop() = 0;
  virtual bool running() const noexcept = 0;
  virtual const AudioCaptureConfig &format() const noexcept = 0;

protected:
  AudioSource() = default;
};

} // namespace darkos::media
