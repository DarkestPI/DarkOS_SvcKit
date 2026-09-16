#pragma once

#include "media_pipeline.h"

#include <memory>
#include <string>

namespace darkos::media {

/**
 * 多路媒体管线创建门面。当前默认实现无全局单例，设备冲突由 Platform 返回。
 * 后续可在实现中加入通道配额、主辅码流共享和资源仲裁。
 */
class MediaManager {
public:
  virtual ~MediaManager() = default;
  virtual std::unique_ptr<MediaPipeline>
  createVideoPipeline(const VideoPipelineConfig &config,
                      PacketCallback callback, std::string &error) = 0;
  virtual std::unique_ptr<MediaPipeline>
  createAvPipeline(const MediaPipelineConfig &config,
                   PacketCallback videoCallback,
                   AudioFrameCallback audioCallback, std::string &error) = 0;

protected:
  MediaManager() = default;
};

std::unique_ptr<MediaManager> createMediaManager();

} // namespace darkos::media
