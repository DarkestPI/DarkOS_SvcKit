#pragma once

#include "media_buffer.h"
#include "media_types.h"

#include <memory>
#include <string>

namespace darkos::media {

class AudioEncoder {
public:
  virtual ~AudioEncoder() = default;

  AudioEncoder(const AudioEncoder &) = delete;
  AudioEncoder &operator=(const AudioEncoder &) = delete;

  virtual int start() = 0;
  virtual int stop() = 0;
  virtual bool running() const noexcept = 0;
  virtual int encode(const AudioFrame &frame, AudioPacketPtr &packet) = 0;
  virtual const AudioEncoderConfig &config() const noexcept = 0;

protected:
  AudioEncoder() = default;
};

class AudioDecoder {
public:
  virtual ~AudioDecoder() = default;

  AudioDecoder(const AudioDecoder &) = delete;
  AudioDecoder &operator=(const AudioDecoder &) = delete;

  virtual int start() = 0;
  virtual int stop() = 0;
  virtual bool running() const noexcept = 0;
  virtual int decode(const AudioPacket &packet, AudioFramePtr &frame) = 0;

protected:
  AudioDecoder() = default;
};

/** 创建内置 PCM/G.711 编码器；AAC/Opus 未接实现时明确失败。 */
std::unique_ptr<AudioEncoder>
createAudioEncoder(const AudioEncoderConfig &config,
                   const AudioCaptureConfig &inputFormat, std::string &error);

/** 创建与 createAudioEncoder 对称的内置 PCM/G.711 解码器。 */
std::unique_ptr<AudioDecoder>
createAudioDecoder(AudioCodec codec, const AudioCaptureConfig &outputFormat,
                   std::string &error);

} // namespace darkos::media
