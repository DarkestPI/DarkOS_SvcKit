#include "media_audio_sink.h"
#include "media_video_sink.h"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace darkos::media {
namespace {

class DefaultVideoProbeSink final : public VideoProbeSink {
public:
  int start() override {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (running_)
      return -EALREADY;
    stats_ = {};
    running_ = true;
    return 0;
  }

  int stop() override {
    const std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    changed_.notify_all();
    return 0;
  }

  int consume(VideoPacketPtr packet) override {
    if (packet == nullptr || packet->buffer == nullptr ||
        packet->buffer->size() == 0)
      return -EINVAL;
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!running_)
      return -EPIPE;
    ++stats_.packetCount;
    stats_.byteCount += packet->buffer->size();
    if (packet->keyframe)
      ++stats_.keyframeCount;
    stats_.lastTimestampNs = packet->timestampNs;
    stats_.lastCodec = packet->codec;
    changed_.notify_all();
    return 0;
  }

  int waitForPackets(std::uint64_t minimum, int timeoutMs) override {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto ready = [&] {
      return stats_.packetCount >= minimum || !running_;
    };
    if (timeoutMs < 0) {
      changed_.wait(lock, ready);
    } else if (!changed_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                  ready)) {
      return -ETIMEDOUT;
    }
    return stats_.packetCount >= minimum ? 0 : -ECANCELED;
  }

  VideoProbeStats snapshot() const noexcept override {
    const std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  VideoProbeStats stats_;
  bool running_{false};
};

class DefaultAudioProbeSink final : public AudioProbeSink {
public:
  int start() override {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (running_)
      return -EALREADY;
    stats_ = {};
    running_ = true;
    return 0;
  }

  int stop() override {
    const std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    changed_.notify_all();
    return 0;
  }

  int consume(AudioPacketPtr packet) override {
    if (packet == nullptr || packet->buffer == nullptr ||
        packet->buffer->size() == 0 || packet->sampleRate == 0 ||
        packet->channelCount == 0)
      return -EINVAL;
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!running_)
      return -EPIPE;
    ++stats_.packetCount;
    stats_.byteCount += packet->buffer->size();
    stats_.lastTimestampNs = packet->timestampNs;
    stats_.lastCodec = packet->codec;
    stats_.lastSampleRate = packet->sampleRate;
    stats_.lastChannelCount = packet->channelCount;
    changed_.notify_all();
    return 0;
  }

  int waitForPackets(std::uint64_t minimum, int timeoutMs) override {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto ready = [&] {
      return stats_.packetCount >= minimum || !running_;
    };
    if (timeoutMs < 0) {
      changed_.wait(lock, ready);
    } else if (!changed_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                  ready)) {
      return -ETIMEDOUT;
    }
    return stats_.packetCount >= minimum ? 0 : -ECANCELED;
  }

  AudioProbeStats snapshot() const noexcept override {
    const std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  AudioProbeStats stats_;
  bool running_{false};
};

} // namespace

std::shared_ptr<VideoProbeSink> createVideoProbeSink() {
  return std::make_shared<DefaultVideoProbeSink>();
}

std::shared_ptr<AudioProbeSink> createAudioProbeSink() {
  return std::make_shared<DefaultAudioProbeSink>();
}

} // namespace darkos::media
