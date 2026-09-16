#include "media_manager.h"

#include <utility>

namespace darkos::media {
namespace {

class DefaultMediaManager final : public MediaManager {
public:
  std::unique_ptr<MediaPipeline>
  createVideoPipeline(const VideoPipelineConfig &config,
                      PacketCallback callback, std::string &error) override {
    return createMediaPipeline(config, std::move(callback), error);
  }

  std::unique_ptr<MediaPipeline> createAvPipeline(
      const MediaPipelineConfig &config, PacketCallback videoCallback,
      AudioFrameCallback audioCallback, std::string &error) override {
    return createMediaPipeline(config, std::move(videoCallback),
                               std::move(audioCallback), error);
  }
};

} // namespace

std::unique_ptr<MediaManager> createMediaManager() {
  return std::make_unique<DefaultMediaManager>();
}

} // namespace darkos::media
