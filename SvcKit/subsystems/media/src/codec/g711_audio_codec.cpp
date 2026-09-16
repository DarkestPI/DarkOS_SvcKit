#include "media_audio_codec.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace darkos::media {
namespace {

std::uint8_t linearToUlaw(std::int16_t sample) {
  constexpr int kBias = 0x84;
  constexpr int kClip = 32635;
  int value = sample;
  const int mask = value < 0 ? 0x7f : 0xff;
  if (value < 0)
    value = -value;
  if (value > kClip)
    value = kClip;
  value += kBias;
  int segment = 7;
  for (int threshold = 0x4000; segment > 0 && (value & threshold) == 0;
       threshold >>= 1)
    --segment;
  const int mantissa = (value >> (segment + 3)) & 0x0f;
  return static_cast<std::uint8_t>((segment << 4 | mantissa) ^ mask);
}

std::int16_t ulawToLinear(std::uint8_t value) {
  value = static_cast<std::uint8_t>(~value);
  int sample = ((value & 0x0f) << 3) + 0x84;
  sample <<= (value & 0x70) >> 4;
  return static_cast<std::int16_t>((value & 0x80) ? 0x84 - sample
                                                  : sample - 0x84);
}

std::uint8_t linearToAlaw(std::int16_t sample) {
  int value = sample;
  int mask;
  if (value >= 0) {
    mask = 0xd5;
  } else {
    mask = 0x55;
    value = -value - 1;
  }
  if (value > 32767)
    value = 32767;

  int encoded;
  if (value < 256) {
    encoded = value >> 4;
  } else {
    int segment = 1;
    for (int shifted = value >> 8; shifted > 1; shifted >>= 1)
      ++segment;
    if (segment > 7)
      segment = 7;
    encoded = (segment << 4) | ((value >> (segment + 3)) & 0x0f);
  }
  return static_cast<std::uint8_t>(encoded ^ mask);
}

std::int16_t alawToLinear(std::uint8_t value) {
  value ^= 0x55;
  int sample = (value & 0x0f) << 4;
  const int segment = (value & 0x70) >> 4;
  if (segment == 0) {
    sample += 8;
  } else if (segment == 1) {
    sample += 0x108;
  } else {
    sample += 0x108;
    sample <<= segment - 1;
  }
  return static_cast<std::int16_t>((value & 0x80) ? sample : -sample);
}

class G711AudioEncoder final : public AudioEncoder {
public:
  G711AudioEncoder(AudioEncoderConfig config, AudioCaptureConfig input)
      : config_(std::move(config)), input_(std::move(input)) {}

  int start() override {
    bool expected = false;
    return running_.compare_exchange_strong(expected, true) ? 0 : -EALREADY;
  }

  int stop() override {
    running_.store(false);
    return 0;
  }

  bool running() const noexcept override { return running_.load(); }

  int encode(const AudioFrameView &frame,
             EncodedAudioPacketView &packet) override {
    if (!running_.load())
      return -EPIPE;
    if (frame.data == nullptr || frame.size % sizeof(std::int16_t) != 0 ||
        frame.sampleFormat != AudioSampleFormat::PcmS16Le ||
        frame.sampleRate != input_.sampleRate ||
        frame.channelCount != input_.channelCount)
      return -EINVAL;

    if (config_.codec == AudioCodec::Pcm) {
      output_.assign(frame.data, frame.data + frame.size);
    } else {
      const std::size_t sampleCount = frame.size / sizeof(std::int16_t);
      output_.resize(sampleCount);
      for (std::size_t index = 0; index < sampleCount; ++index) {
        std::int16_t sample;
        std::memcpy(&sample, frame.data + index * sizeof(sample),
                    sizeof(sample));
        output_[index] = config_.codec == AudioCodec::G711A
                             ? linearToAlaw(sample)
                             : linearToUlaw(sample);
      }
    }
    packet = EncodedAudioPacketView{output_.data(),    output_.size(),
                                    frame.timestampNs, config_.codec,
                                    frame.sampleRate,  frame.channelCount};
    return 0;
  }

  const AudioEncoderConfig &config() const noexcept override { return config_; }

private:
  AudioEncoderConfig config_;
  AudioCaptureConfig input_;
  std::vector<std::uint8_t> output_;
  std::atomic<bool> running_{false};
};

class G711AudioDecoder final : public AudioDecoder {
public:
  G711AudioDecoder(AudioCodec codec, AudioCaptureConfig output)
      : codec_(codec), outputFormat_(std::move(output)) {}

  int start() override {
    bool expected = false;
    return running_.compare_exchange_strong(expected, true) ? 0 : -EALREADY;
  }

  int stop() override {
    running_.store(false);
    return 0;
  }

  bool running() const noexcept override { return running_.load(); }

  int decode(const EncodedAudioPacketView &packet,
             AudioFrameView &frame) override {
    if (!running_.load())
      return -EPIPE;
    if (packet.data == nullptr || packet.codec != codec_ ||
        packet.sampleRate != outputFormat_.sampleRate ||
        packet.channelCount != outputFormat_.channelCount)
      return -EINVAL;

    if (codec_ == AudioCodec::Pcm) {
      output_.assign(packet.data, packet.data + packet.size);
    } else {
      if (packet.size >
          std::numeric_limits<std::size_t>::max() / sizeof(std::int16_t))
        return -EOVERFLOW;
      output_.resize(packet.size * sizeof(std::int16_t));
      for (std::size_t index = 0; index < packet.size; ++index) {
        const std::int16_t sample = codec_ == AudioCodec::G711A
                                        ? alawToLinear(packet.data[index])
                                        : ulawToLinear(packet.data[index]);
        std::memcpy(output_.data() + index * sizeof(sample), &sample,
                    sizeof(sample));
      }
    }
    const std::uint32_t bytesPerFrame =
        outputFormat_.channelCount * sizeof(std::int16_t);
    frame = AudioFrameView{
        output_.data(),
        output_.size(),
        packet.timestampNs,
        outputFormat_.sampleRate,
        outputFormat_.channelCount,
        static_cast<std::uint32_t>(output_.size() / bytesPerFrame),
        AudioSampleFormat::PcmS16Le};
    return 0;
  }

private:
  AudioCodec codec_;
  AudioCaptureConfig outputFormat_;
  std::vector<std::uint8_t> output_;
  std::atomic<bool> running_{false};
};

bool supported(AudioCodec codec) {
  return codec == AudioCodec::Pcm || codec == AudioCodec::G711A ||
         codec == AudioCodec::G711U;
}

} // namespace

std::unique_ptr<AudioEncoder>
createAudioEncoder(const AudioEncoderConfig &config,
                   const AudioCaptureConfig &inputFormat, std::string &error) {
  if (!supported(config.codec)) {
    error = "requested audio encoder is not implemented";
    return nullptr;
  }
  if (inputFormat.sampleFormat != AudioSampleFormat::PcmS16Le ||
      inputFormat.sampleRate == 0 || inputFormat.channelCount == 0) {
    error = "audio encoder input format is invalid";
    return nullptr;
  }
  return std::make_unique<G711AudioEncoder>(config, inputFormat);
}

std::unique_ptr<AudioDecoder>
createAudioDecoder(AudioCodec codec, const AudioCaptureConfig &outputFormat,
                   std::string &error) {
  if (!supported(codec)) {
    error = "requested audio decoder is not implemented";
    return nullptr;
  }
  if (outputFormat.sampleFormat != AudioSampleFormat::PcmS16Le ||
      outputFormat.sampleRate == 0 || outputFormat.channelCount == 0) {
    error = "audio decoder output format is invalid";
    return nullptr;
  }
  return std::make_unique<G711AudioDecoder>(codec, outputFormat);
}

} // namespace darkos::media
