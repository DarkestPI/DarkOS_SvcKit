#include "media_pipeline.h"

#include "media_audio_source.h"
#include "media_factory.h"
#include "media_video_codec.h"
#include "media_video_source.h"

#include <svc_log.h>

#include <atomic>
#include <cerrno>
#include <exception>
#include <memory>
#include <mutex>
#include <utility>

namespace darkos::media {
namespace {

constexpr char kTag[] = "svc_media";

class NodeMediaPipeline final : public MediaPipeline {
public:
  NodeMediaPipeline(std::unique_ptr<VideoSource> videoSource,
                    std::unique_ptr<VideoEncoder> videoEncoder,
                    std::unique_ptr<AudioSource> audioSource,
                    PacketCallback videoCallback,
                    AudioFrameCallback audioCallback)
      : videoSource_(std::move(videoSource)),
        videoEncoder_(std::move(videoEncoder)),
        audioSource_(std::move(audioSource)),
        videoCallback_(std::move(videoCallback)),
        audioCallback_(std::move(audioCallback)) {}

  ~NodeMediaPipeline() override { stop(); }

  int start() override {
    const std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (running_.load())
      return -EALREADY;

    int rc = videoEncoder_->start();
    if (rc != 0)
      return rc;
    running_.store(true);

    if (audioSource_ != nullptr) {
      rc = audioSource_->start([this](const AudioFrameView &frame) {
        if (running_.load())
          audioCallback_(frame);
      });
      if (rc != 0) {
        running_.store(false);
        videoEncoder_->stop();
        return rc;
      }
    }

    rc = videoSource_->start(
        [this](const VideoFrameView &frame) { onVideoFrame(frame); });
    if (rc != 0) {
      running_.store(false);
      if (audioSource_ != nullptr)
        audioSource_->stop();
      videoEncoder_->stop();
      return rc;
    }
    return 0;
  }

  int stop() override {
    const std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (!running_.exchange(false))
      return 0;

    const int videoSourceResult = videoSource_->stop();
    const int audioSourceResult =
        audioSource_ != nullptr ? audioSource_->stop() : 0;
    const int encoderResult = videoEncoder_->stop();
    if (videoSourceResult != 0)
      return videoSourceResult;
    return audioSourceResult != 0 ? audioSourceResult : encoderResult;
  }

  bool running() const noexcept override { return running_.load(); }

private:
  void onVideoFrame(const VideoFrameView &frame) noexcept {
    if (!running_.load())
      return;
    EncodedPacketView packet;
    const int rc = videoEncoder_->encode(frame, packet);
    if (rc != 0) {
      SVC_LOGE(kTag, "video encode failed: %d", rc);
      return;
    }
    if (packet.size == 0)
      return;
    try {
      videoCallback_(packet);
    } catch (const std::exception &exception) {
      SVC_LOGE(kTag, "video packet callback threw: %s", exception.what());
    } catch (...) {
      SVC_LOGE(kTag, "video packet callback threw an unknown exception");
    }
  }

  std::unique_ptr<VideoSource> videoSource_;
  std::unique_ptr<VideoEncoder> videoEncoder_;
  std::unique_ptr<AudioSource> audioSource_;
  PacketCallback videoCallback_;
  AudioFrameCallback audioCallback_;
  std::atomic<bool> running_{false};
  std::mutex lifecycleMutex_;
};

std::unique_ptr<MediaPipeline>
createPipelineNodes(const VideoPipelineConfig &videoConfig,
                    const AudioCaptureConfig *audioConfig,
                    PacketCallback videoCallback,
                    AudioFrameCallback audioCallback, std::string &error) {
  if (!videoCallback) {
    error = "video packet callback is required";
    return nullptr;
  }
  if (audioConfig != nullptr && !audioCallback) {
    error = "audio frame callback is required";
    return nullptr;
  }

  auto videoSource = createPlatformVideoSource(videoConfig.capture, error);
  if (videoSource == nullptr)
    return nullptr;
  auto videoEncoder =
      createPlatformVideoEncoder(videoConfig.encoder, videoSource->format(),
                                 videoConfig.encodedBufferCapacity, error);
  if (videoEncoder == nullptr)
    return nullptr;

  std::unique_ptr<AudioSource> audioSource;
  if (audioConfig != nullptr) {
    audioSource = createPlatformAudioSource(*audioConfig, error);
    if (audioSource == nullptr)
      return nullptr;
  }
  return std::make_unique<NodeMediaPipeline>(
      std::move(videoSource), std::move(videoEncoder), std::move(audioSource),
      std::move(videoCallback), std::move(audioCallback));
}

} // namespace

std::unique_ptr<MediaPipeline>
createMediaPipeline(const VideoPipelineConfig &config, PacketCallback callback,
                    std::string &error) {
  return createPipelineNodes(config, nullptr, std::move(callback), {}, error);
}

std::unique_ptr<MediaPipeline>
createMediaPipeline(const MediaPipelineConfig &config,
                    PacketCallback videoCallback,
                    AudioFrameCallback audioCallback, std::string &error) {
  return createPipelineNodes(config.video, &config.audio,
                             std::move(videoCallback), std::move(audioCallback),
                             error);
}

} // namespace darkos::media
