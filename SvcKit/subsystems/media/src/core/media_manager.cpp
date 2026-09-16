#include "media_manager.h"

namespace darkos::media {
namespace {

class DefaultMediaManager final : public MediaManager {
public:
  std::unique_ptr<MediaPipeline>
  createVideoPipeline(const VideoPipelineConfig &config,
                      std::string &error) override {
    return createMediaPipeline(config, error);
  }

  std::unique_ptr<MediaPipeline>
  createAvPipeline(const MediaPipelineConfig &config,
                   std::string &error) override {
    return createMediaPipeline(config, error);
  }
};

} // namespace

std::unique_ptr<MediaManager> createMediaManager() {
  return std::make_unique<DefaultMediaManager>();
}

} // namespace darkos::media
